#pragma once

#include <atomic>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <d3d12.h>

namespace uevr::nascar26 {
constexpr bool matches_name(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    path = path.substr(slash == path.npos ? 0 : slash + 1);
    constexpr std::wstring_view name = L"nascar26_steam-win64-shipping.exe";
    if (path.size() != name.size()) { return false; }
    for (size_t i = 0; i < name.size(); ++i) {
        const auto c = path[i] >= L'A' && path[i] <= L'Z' ? path[i] + (L'a' - L'A') : path[i];
        if (c != name[i]) { return false; }
    }
    return true;
}

constexpr bool intersects_image(uintptr_t address, size_t bytes, uintptr_t base, size_t size) {
    if (!bytes || address > UINTPTR_MAX - bytes || base > UINTPTR_MAX - size) { return true; }
    return size && address < base + size && base < address + bytes;
}

constexpr bool matches_build_metadata(uint32_t version_ms, uint32_t version_ls, uint32_t timestamp, size_t image_size) {
    return version_ms == 0x50007 && version_ls == 0x40000 && timestamp == 0xb910822a && image_size == 0xc14b000;
}

constexpr bool supports_rendering_method(int method, bool extreme) {
    return !extreme && (method == 0 || method == 1); // Native or Synced, never Alternating/DIBR.
}

constexpr bool needs_generic_renderer_hook(bool code_preserving_target, bool native_fix, bool dibr, bool splitscreen) {
    return !code_preserving_target && (native_fix || dibr || splitscreen);
}

struct RedrawIdentity {
    uintptr_t engine{}, client{}, viewport{}, world{}, target{};
    uint64_t lifecycle{};
    uint32_t width{}, height{};
    bool operator==(const RedrawIdentity&) const = default;
    constexpr bool valid() const {
        return engine && client && viewport && world && target && width && height && width <= 16384 && height <= 16384;
    }
};

// One pending second eye, owned exclusively by the game thread. Consume before
// calling Draw so a reentrant callback cannot duplicate or retain the ticket.
class SyncedRedraw {
public:
    struct Ticket { RedrawIdentity identity; uint64_t frame{}, observed_ms{}; };
    bool schedule(RedrawIdentity identity, uint64_t frame, uint64_t now, bool eligible) {
        if (!eligible || !identity.valid() || !now || !(frame & 1) || frame == UINT64_MAX) { reset(); return false; }
        if (m_pending) { return false; }
        m_pending = Ticket{identity, frame, now};
        return true;
    }
    std::optional<Ticket> take(RedrawIdentity current, uint64_t frame, uint64_t now, bool eligible) {
        const auto pending = m_pending;
        reset();
        if (!pending || !eligible || !current.valid() || current != pending->identity ||
            frame != pending->frame || now < pending->observed_ms || now - pending->observed_ms > 500) { return {}; }
        return pending;
    }
    bool pending() const { return m_pending.has_value(); }
    void reset() { m_pending.reset(); }
private:
    std::optional<Ticket> m_pending;
};

constexpr bool completed_synced_redraw(const RedrawIdentity& expected, const RedrawIdentity& current,
    uint64_t expected_frame, uint64_t observed_frame, uint64_t serial_before, uint64_t serial_after, bool eligible) {
    return eligible && expected.valid() && expected == current && expected_frame == observed_frame &&
        serial_before != UINT64_MAX && serial_after == serial_before + 1;
}

struct GhostOwner {
    RedrawIdentity draw{};
    uintptr_t instance{}, player{}, controller{}, left_state{};
    int32_t object_index{-1};
    bool operator==(const GhostOwner&) const = default;
    bool valid() const {
        return draw.valid() && instance && player && controller && left_state && object_index >= 0;
    }
};

// Game-thread-only. No retained UObject is dereferenced to establish ownership.
class GhostOwnerGate {
public:
    bool observe(const GhostOwner& current, uint64_t frame, uint64_t now) {
        if (!current.valid() || !now) { reset(); return false; }
        if (current != owner || frame < last_frame || now < last_ms || now - last_ms > 500) {
            owner = current;
            stable_frames = 0;
            // A pause or frame discontinuity is not permission for more allocations.
            if (current != allocation_owner) { allocation_owner = current; attempts = 0; next_attempt = 0; }
        }
        if (frame != last_frame || stable_frames == 0) { stable_frames = std::min<uint32_t>(12, stable_frames + 1); }
        last_frame = frame;
        last_ms = now;
        return stable_frames >= 12;
    }
    bool begin_bootstrap(uint64_t frame, bool requested, bool right_eye, bool missing) {
        if (!requested || !right_eye || !missing || stable_frames < 12 || attempts >= 3 ||
            frame < next_attempt || frame > UINT64_MAX - 30) { return false; }
        ++attempts;
        next_attempt = frame + 30;
        return true;
    }
    void reset() { owner = {}; stable_frames = 0; last_frame = last_ms = 0; }
    GhostOwner owner{};
    uint32_t stable_frames{};
    uint8_t attempts{};
private:
    GhostOwner allocation_owner{};
    uint64_t last_frame{}, last_ms{}, next_attempt{};
};

struct GhostCall {
    GhostOwner owner{};
    uintptr_t family{}, options{};
    uint64_t frame{};
    uintptr_t selected_state{};
    uint32_t init_calls{}, projection_calls{};
    bool right{}, bootstrap{}, prepared{};
    bool normalize_projection(uintptr_t player, uintptr_t viewport, uintptr_t data, int32_t index) const {
        return bootstrap && player == owner.player && viewport == owner.draw.viewport &&
            data == options && index == 1;
    }
};

class GhostCallScope {
public:
    GhostCallScope(GhostCall*& slot, GhostCall* current) : m_slot{slot}, m_previous{slot} { m_slot = current; }
    ~GhostCallScope() { m_slot = m_previous; }
    GhostCallScope(const GhostCallScope&) = delete;
    GhostCallScope& operator=(const GhostCallScope&) = delete;
private:
    GhostCall*& m_slot;
    GhostCall* m_previous;
};

enum class GhostStatus { Off, WaitingOwner, WaitingBootstrap, Learning, Active, FailedClosed };

struct NativeOwnedObject {
    uintptr_t object{}, vtable{};
    int32_t index{-1};
};
constexpr uint32_t native_owner_root_flag = 1u << 30;
constexpr uintptr_t native_owner_set_flags_rva = 0x150e680;
bool native_owned_object_valid(const NativeOwnedObject& owner, uintptr_t item, bool require_root = true);
bool validate_native_rooting();

using NativeRect = std::array<int32_t, 4>;
struct NativeView {
    uintptr_t view{}, family{}, state{};
    uint64_t frame{}, projection_hash{};
    int32_t index{-1}, pass{}, primary_index{-1}, player_index{-1};
    NativeRect rect{}, constrained{};
    bool valid{};
};
struct NativeCall {
    GhostOwner owner{};
    NativeView view{};
    uintptr_t options{};
    uint32_t init_calls{}, projection_calls{};
};

// UE5.7.4 inserts RenderTargetDepth between RenderTarget and Scene.
struct NativeFamilyTargets {
    uintptr_t color{}, depth{}, scene{};
    bool operator==(const NativeFamilyTargets&) const = default;
};
constexpr size_t native_family_targets_offset = 0x30;
static_assert(sizeof(NativeFamilyTargets) == 0x18 && offsetof(NativeFamilyTargets, scene) == 0x10);
std::optional<NativeFamilyTargets> read_native_family_targets(uintptr_t family);
constexpr bool native_main_family_targets_valid(const NativeFamilyTargets& targets, uintptr_t viewport, uintptr_t world_scene) {
    return viewport && world_scene && targets.color == viewport && targets.scene == world_scene && targets.depth == 0;
}

class NativeCallScope {
public:
    NativeCallScope(NativeCall*& slot, NativeCall* current) : m_slot{slot}, m_previous{slot} { m_slot = current; }
    ~NativeCallScope() { m_slot = m_previous; }
    NativeCallScope(const NativeCallScope&) = delete;
    NativeCallScope& operator=(const NativeCallScope&) = delete;
private:
    NativeCall*& m_slot;
    NativeCall* m_previous;
};

inline bool native_projection_valid(std::span<const double, 16> matrix) {
    return std::all_of(matrix.begin(), matrix.end(), [](double v) { return std::isfinite(v); }) &&
        matrix[0] > 0 && matrix[5] > 0 && matrix[11] == 1 && matrix[15] == 0;
}
constexpr bool native_rect_valid(const NativeRect& rect, const NativeRect& constrained, uint32_t width, uint32_t height) {
    return width && height && width <= 16384 && height <= 16384 &&
        rect == NativeRect{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)} &&
        constrained[0] >= 0 && constrained[1] >= 0 && constrained[0] < constrained[2] && constrained[1] < constrained[3] &&
        constrained[2] <= static_cast<int32_t>(width) && constrained[3] <= static_cast<int32_t>(height);
}
inline bool native_pair_valid(const std::array<NativeView, 2>& views, uintptr_t family,
    uintptr_t left_state, uintptr_t right_state, uint64_t frame, uint32_t width, uint32_t height) {
    if (!family || !left_state || !right_state || left_state == right_state ||
        !views[0].view || !views[1].view || views[0].view == views[1].view) { return false; }
    for (int32_t eye = 0; eye != 2; ++eye) {
        const auto& view = views[eye];
        if (!view.valid || view.family != family || view.frame != frame || view.index != eye ||
            view.pass != eye + 1 || view.primary_index != 0 || view.player_index != 0 || !view.projection_hash ||
            view.state != (eye ? right_state : left_state) || !native_rect_valid(view.rect, view.constrained, width, height)) { return false; }
    }
    return true;
}

struct NativePairKey {
    GhostOwner owner{};
    uintptr_t scene{}, right_state{}, render_target{};
    uint64_t generation{};
    uint32_t width{}, height{};
    bool operator==(const NativePairKey&) const = default;
};
class NativePairGate {
public:
    bool observe(const NativePairKey& key, uint64_t frame, bool valid) {
        if (!valid || frame == UINT64_MAX || !key.owner.valid() || !key.scene || !key.right_state || !key.render_target || !key.generation ||
            !key.width || !key.height || key.width > 16384 || key.height > 16384) {
            reset(); return false;
        }
        if (key != m_key || (m_count && frame != m_frame && frame != m_frame + 1)) { reset(); m_key = key; }
        if (!m_count || frame != m_frame) { m_count = std::min<uint32_t>(3, m_count + 1); }
        m_frame = frame;
        return m_count >= 3;
    }
    void reset() { m_key = {}; m_count = 0; m_frame = 0; }
private:
    NativePairKey m_key{};
    uint64_t m_frame{};
    uint32_t m_count{};
};

// The caller validates writable storage. Preserve eye index, matrices, state,
// and every other field while the engine copies this secondary singleton.
class NativeSingletonScope {
public:
    bool apply(void* view, uintptr_t family, uintptr_t replacement) {
        if (m_view || !view || !family || !replacement) { return false; }
        auto* bytes = static_cast<uint8_t*>(view);
        std::memcpy(&m_family, bytes + 8, sizeof(m_family));
        std::memcpy(&m_pass, bytes + 0xdd0, sizeof(m_pass));
        std::memcpy(&m_primary, bytes + 0xdd8, sizeof(m_primary));
        if (m_family != family || m_pass != 2 || m_primary != 0) { return false; }
        m_view = bytes;
        const int32_t primary_pass = 1;
        std::memcpy(bytes + 8, &replacement, sizeof(replacement));
        std::memcpy(bytes + 0xdd0, &primary_pass, sizeof(primary_pass));
        return true;
    }
    void restore() {
        if (!m_view) { return; }
        std::memcpy(m_view + 8, &m_family, sizeof(m_family));
        std::memcpy(m_view + 0xdd0, &m_pass, sizeof(m_pass));
        m_view = nullptr;
    }
    ~NativeSingletonScope() { restore(); }
    NativeSingletonScope() = default;
    NativeSingletonScope(const NativeSingletonScope&) = delete;
    NativeSingletonScope& operator=(const NativeSingletonScope&) = delete;
private:
    uint8_t* m_view{};
    uintptr_t m_family{};
    int32_t m_pass{}, m_primary{};
};

inline void release_borrowed_native_interfaces(void* family, const std::array<uintptr_t, 4>& borrowed) {
    for (size_t i = 0; i < borrowed.size(); ++i) {
        uintptr_t current{};
        auto* slot = static_cast<uint8_t*>(family) + 0x160 + i * sizeof(uintptr_t);
        std::memcpy(&current, slot, sizeof(current));
        if (current && current == borrowed[i]) { std::memset(slot, 0, sizeof(current)); }
    }
}

constexpr bool ghost_pair_valid(uintptr_t left, uintptr_t right) { return left && right && left != right; }
constexpr bool ghost_consumer_matches(uintptr_t family, uintptr_t expected_family,
    uintptr_t state, uintptr_t expected_state, int32_t pass, int32_t index) {
    return family && family == expected_family && state && state == expected_state && pass == 1 && index == 0;
}

// Only the two labels differ during lazy allocation of ViewStates[1]. Restore
// them before FSceneView construction; projection remains the ordinary index-0 path.
inline void restore_ghost_bootstrap_labels(void* options) {
    const int32_t primary = 1, index = 0;
    std::memcpy(static_cast<uint8_t*>(options) + 0x1c0, &primary, sizeof(primary));
    std::memcpy(static_cast<uint8_t*>(options) + 0x1c4, &index, sizeof(index));
}

constexpr bool valid_texture_desc(const D3D12_RESOURCE_DESC& d, uint32_t width, uint32_t height, bool ui) {
    if (!width || !height || width > 16384 || height > 16384 ||
        d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != width || d.Height != height ||
        d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 || d.SampleDesc.Quality != 0 ||
        !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))) { return false; }
    const bool rgba8 = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return rgba8 || (!ui && (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS));
}

constexpr bool scene_observation_fresh(uint64_t now, uint64_t observed) {
    return observed != 0 && now >= observed && now - observed <= 500;
}

// Existing OpenXR/OpenVR destinations are BGRA8. Other legal color formats
// require a conversion pass; do not feed them to the ordinary copy path.
constexpr bool copy_compatible_format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

struct SceneIdentity {
    uintptr_t viewport{}, texture{}, native{};
    uint32_t width{}, height{}, format{};
    bool operator==(const SceneIdentity&) const = default;
};

struct WindowSize { float x{}, y{}; };
static_assert(sizeof(WindowSize) == sizeof(uint64_t));

// Render-thread-only gate; a changed or invalid resource must warm up again.
class SceneStability {
public:
    bool observe(SceneIdentity current) {
        if (!current.viewport || !current.texture || !current.native || !current.width || !current.height || !current.format) {
            reset();
            return false;
        }
        if (current != m_pending) { m_pending = current; return false; }
        return true;
    }
    void reset() { m_pending = {}; }
private:
    SceneIdentity m_pending{};
};

// Does not modify the game's code, image vtables or integrity checker. Unknown images fail closed.
void initialize();
bool is_target();
bool is_validated_build();
uintptr_t image_base();
bool read_memory(uintptr_t address, void* out, size_t bytes);
bool matches_code(uintptr_t address, std::span<const uint8_t> expected);
bool validate_synced_redraw();
bool validate_ghost_view_setup();
bool validate_native_renderer();
uintptr_t find_virtual_slot(uintptr_t table, uintptr_t expected, size_t count);
uintptr_t find_virtual_slot_in_image(uintptr_t table, uintptr_t expected, size_t count, uintptr_t image);
bool writable_object_pointer(uintptr_t address);
bool protect_external_memory(uint8_t* address, size_t bytes, uint32_t protection,
    uint32_t* old, uintptr_t image, size_t image_size);

// Only the validated Slate callsite reads this borrowed view, at +8, then
// immediately acquires its own RHI reference. The view never escapes that call.
struct SlateResourceView {
    uintptr_t unused_vtable{};
    uintptr_t texture{};
};
static_assert(sizeof(SlateResourceView) == 16 && offsetof(SlateResourceView, texture) == 8);

constexpr uintptr_t engine_global_rva = 0xa2815c0;
constexpr uintptr_t engine_vtable_rva = 0x8506718;
constexpr size_t engine_slots = 175;
constexpr uintptr_t viewport_dispatch_vtable_rva = 0x8a08170;
constexpr size_t viewport_dispatch_slots = 61;
constexpr uintptr_t viewport_client_subobject = 0x28;
constexpr uintptr_t tick_rva = 0x3b34970;
constexpr uintptr_t draw_rva = 0x3b58020;
constexpr uintptr_t viewport_draw_rva = 0x41ba530;
constexpr uintptr_t viewport_vtable_rva = 0x86d9958;
constexpr uintptr_t localplayer_vtable_rva = 0x85c7bd0;
constexpr size_t localplayer_slots = 116;
constexpr uintptr_t init_options_rva = 0x3cc9130, calc_scene_view_rva = 0x3cc88a0, projection_data_rva = 0x3cd1f20;
constexpr uintptr_t init_options_return_rva = 0x3cc8b08, projection_data_return_rva = 0x3cc9310;
constexpr uintptr_t calc_scene_view_return_rva = 0x3b58946;
constexpr uintptr_t view_state_vtable_rva = 0x8027558, view_vtable_rva = 0x8026d80;
constexpr uintptr_t renderer_global_rva = 0xa250d10, renderer_vtable_rva = 0x8033248;
constexpr size_t renderer_slots = 76;
constexpr uintptr_t renderer_single_rva = 0x2b09dd0, renderer_plural_rva = 0x2b09680;
constexpr uintptr_t renderer_return_rva = 0x3b5969b;
constexpr uintptr_t family_copy_rva = 0x3ff5cc0, family_delete_rva = 0x3ff95b0;
constexpr uintptr_t family_vtable_rva = 0x86b3cd0, family_context_vtable_rva = 0x86b3cd8;
constexpr uintptr_t stereo_vtable_rva = 0x8769fc0;
constexpr size_t stereo_slots = 21;
constexpr uintptr_t slate_vtable_rva = 0x86d9b78;
constexpr size_t slate_slots = 46;
constexpr uintptr_t slate_getter_rva = 0x40c8690;
constexpr uintptr_t slate_getter_return_rva = 0x30578a5;
constexpr uintptr_t slate_subobject_from_viewport = 0xc0;

struct SlotPatch {
    size_t index;
    uintptr_t expected;
    uintptr_t replacement;
};

class ObjectVTable {
public:
    ObjectVTable() = default;
    ObjectVTable(const ObjectVTable&) = delete;
    ObjectVTable& operator=(const ObjectVTable&) = delete;
    ~ObjectVTable();

    bool prepare(uintptr_t table, size_t count, std::span<const SlotPatch> patches, uintptr_t image);
    bool install(uintptr_t object);
    bool reset();
    bool active() const { return m_active.load(std::memory_order_acquire); }
    bool prepared() const { return m_prepared.load(std::memory_order_acquire); }
    bool image_unchanged() const;
    bool owns(uintptr_t object) const;
    uintptr_t original_address() const { return prepared() ? original_address(m_primary_slot) : 0; }
    uintptr_t original_address(size_t index) const {
        return prepared() && index < m_count ? m_original[index] : 0;
    }
    uintptr_t slot_address() const { return m_object.load(std::memory_order_acquire); }
    template<class Ret = void, class... Args> Ret call(Args... args) const {
        return reinterpret_cast<Ret (*)(Args...)>(original_address())(args...);
    }
    template<class Ret = void, class... Args> Ret call_slot(size_t index, Args... args) const {
        return reinterpret_cast<Ret (*)(Args...)>(original_address(index))(args...);
    }

private:
    std::mutex m_mutex;
    std::atomic_uintptr_t m_object{};
    std::atomic_bool m_active{};
    std::atomic_bool m_prepared{};
    std::array<uintptr_t, 256> m_original{};
    uintptr_t* m_shadow{};
    uintptr_t m_table{};
    size_t m_count{};
    size_t m_primary_slot{};
    size_t m_patch_count{};
};
}
