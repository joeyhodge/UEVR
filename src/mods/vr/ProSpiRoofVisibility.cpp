#define NOMINMAX
#include "ProSpiRoofVisibility.hpp"
#include "ProSpiRoofVisibilityPolicy.hpp"
#include "ProSpiRoofVisibilityCode.hpp"

#include <Windows.h>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UGameViewportClient.hpp>
#include <sdk/FArrayProperty.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/TArray.hpp>
#include <safetyhook.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace uevr::prospi::roof {
namespace {
using Identity = sdk::object_liveness::Identity;
using Native = void(*)(sdk::UObjectBase*, bool, uint8_t);
using Clock = std::chrono::steady_clock;
constexpr uint32_t non_runtime_flags = 0x10 | 0x20 | 0x200 | 0x400 | 0x1000 | 0x2000 | 0x8000 | 0x10000;

bool read_bytes(uintptr_t address, void* out, size_t bytes) {
    SIZE_T copied{};
    return address && address <= UINTPTR_MAX - bytes &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, bytes, &copied) && copied == bytes;
}
template<class T> bool read(uintptr_t address, T& value) { return read_bytes(address, &value, sizeof(T)); }
sdk::UObjectBase* object(const Identity& id) { return reinterpret_cast<sdk::UObjectBase*>(id.object); }
std::optional<Identity> observe(sdk::UObjectBase* value) {
    const auto observation = sdk::observe_uobject(value);
    if (!observation || !sdk::is_current_object(observation->identity)) { return {}; }
    return observation->identity;
}
bool runtime(const Identity& id) {
    uint32_t flags{};
    return sdk::is_current_object(id) &&
        read(id.object + sdk::UObjectBase::get_object_flags_offset(), flags) && !(flags & non_runtime_flags);
}
bool has_class(const Identity& id, const Identity& cls) {
    uintptr_t value{};
    return read(id.object + sdk::UObjectBase::get_class_private_offset(), value) && value == cls.object;
}
std::optional<Identity> outer(const Identity& id) {
    sdk::UObjectBase* value{};
    return read(id.object + sdk::UObjectBase::get_outer_private_offset(), value) ? observe(value) : std::nullopt;
}

struct Boolean {
    uint32_t offset{};
    uint8_t mask{};
    bool get(const Identity& id, bool& value) const {
        uint8_t byte{};
        if (!mask || !read(id.object + offset, byte)) { return false; }
        value = (byte & mask) != 0;
        return true;
    }
};
bool property_type(sdk::FProperty* prop, std::wstring_view type) {
    return prop && prop->get_class() && prop->get_class()->get_name().to_string() == type;
}
std::optional<uint32_t> field(sdk::UStruct* cls, const wchar_t* name, std::wstring_view type, size_t size) {
    const auto prop = cls->find_property(name);
    if (!property_type(prop, type)) { return {}; }
    const auto offset = prop->get_offset();
    const auto total = cls->get_properties_size();
    if (offset < 0 || total <= 0 || total > 0x10000 || size > static_cast<size_t>(total) ||
        static_cast<size_t>(offset) > static_cast<size_t>(total) - size) { return {}; }
    return static_cast<uint32_t>(offset);
}
std::optional<Boolean> boolean(sdk::UStruct* cls, const wchar_t* name) {
    const auto offset = field(cls, name, L"BoolProperty", 1);
    if (!offset) { return {}; }
    const auto prop = static_cast<sdk::FBoolProperty*>(cls->find_property(name));
    const auto address = *offset + prop->get_byte_offset();
    const auto mask = prop->get_byte_mask();
    if (!std::has_single_bit(mask) || address >= static_cast<uint32_t>(cls->get_properties_size())) { return {}; }
    return Boolean{address, mask};
}
std::optional<Identity> find_class(const wchar_t* name) {
    return observe(sdk::find_uobject<sdk::UClass>(name));
}
sdk::UClass* klass(const Identity& id) { return static_cast<sdk::UClass*>(object(id)); }

bool module_data(uintptr_t address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    return address && bytes && address <= UINTPTR_MAX - bytes &&
        VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) &&
        info.AllocationBase == GetModuleHandleW(nullptr) && info.Type == MEM_IMAGE && info.State == MEM_COMMIT &&
        !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        address >= reinterpret_cast<uintptr_t>(info.BaseAddress) &&
        address - reinterpret_cast<uintptr_t>(info.BaseAddress) <= info.RegionSize &&
        bytes <= info.RegionSize - (address - reinterpret_cast<uintptr_t>(info.BaseAddress));
}

// Windows unwind metadata bounds every read and rejects mid-function entries.
std::optional<std::pair<uintptr_t, size_t>> function_range(uintptr_t address) {
    DWORD64 base{};
    const auto entry = RtlLookupFunctionEntry(address, &base, nullptr);
    if (!entry || base != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) ||
        base + entry->BeginAddress != address || entry->EndAddress <= entry->BeginAddress) { return {}; }
    const auto size = entry->EndAddress - entry->BeginAddress;
    MEMORY_BASIC_INFORMATION info{};
    if (size > 4096 || !VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
        info.AllocationBase != reinterpret_cast<void*>(base) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) { return {}; }
    return std::pair{address, static_cast<size_t>(size)};
}
std::optional<uintptr_t> resolve_setter(sdk::UFunction* fn, const Boolean& visible) {
    const auto entry = reinterpret_cast<uintptr_t>(fn->get_native_function());
    const auto range = function_range(entry);
    if (!range || range->second > 512) { return {}; }
    std::array<uint8_t, 512> bytes{};
    if (!read_bytes(entry, bytes.data(), range->second)) { return {}; }
    std::optional<uintptr_t> result;
    for (size_t at{}; at < range->second;) {
        INSTRUX ix{};
        if (!ND_SUCCESS(NdDecodeEx(&ix, bytes.data() + at, range->second - at, ND_CODE_64, ND_DATA_64)) || !ix.Length) { return {}; }
        if (ix.Length == 5 && bytes[at] == 0xE8) {
            int32_t rel{};
            std::memcpy(&rel, bytes.data() + at + 1, sizeof(rel));
            const auto target = entry + at + 5 + static_cast<int64_t>(rel);
            if (const auto candidate = function_range(target)) {
                std::array<uint8_t, 256> code{};
                const auto size = (std::min)(code.size(), candidate->second);
                if (read_bytes(target, code.data(), size) && visibility_setter({code.data(), size}, visible.offset, visible.mask)) {
                    if (result && *result != target) { return {}; }
                    result = target;
                }
            }
        }
        at += ix.Length;
    }
    return result;
}
}

struct VisibilityGuard::Impl : std::enable_shared_from_this<Impl> {
    struct Entry {
        Identity component, owner, level, mesh, package;
        Lease lease;
        VisibilityProof proof;
    };
    static inline std::atomic<std::shared_ptr<Impl>> active;
    static inline std::atomic<Native> native_entry{};
    std::atomic<bool> enabled{};
    std::atomic<DWORD> thread{};
    safetyhook::InlineHook hook;
    Native original{};
    std::array<Identity, 8> classes{};
    Identity component_class, actor_class, mesh_class, level_class, world_class;
    Identity viewport_class{}, viewport{}, world{};
    Identity engine{}, engine_class{};
    uint32_t engine_viewport_offset{}, world_offset{}, levels_offset{}, mesh_offset{};
    uintptr_t dispatch_offset{}, viewport_vtable{}, interface_vtable{}, draw_function{};
    Boolean visible{}, hidden{}, actor_hidden{};
    std::unordered_map<uintptr_t, Entry> entries;
    std::vector<uintptr_t> levels, levels_scratch;
    size_t maintenance_cursor{};
    int32_t scan_cursor{};
    Clock::time_point next_scan{};
    uint64_t override_count{};
    InitializationState initialization;

    static void detour(sdk::UObjectBase* component, bool requested, uint8_t propagation) {
        auto self = active.load(std::memory_order_acquire);
        if (!self) {
            // disable() restores this entry before the owner is unpublished.
            if (const auto fn = native_entry.load(std::memory_order_acquire)) { fn(component, requested, propagation); }
            return;
        }
        // Never hold a hook/cache lock over the original: SetVisibility may
        // recursively enter this detour when propagating to attached children.
        self->original(component, requested, propagation);
        if (!self->enabled.load(std::memory_order_acquire) || GetCurrentThreadId() != self->thread.load()) { return; }
        try { self->game_request(component, requested); } catch (...) { /* Preserve the original game call. */ }
    }

    bool initialize(sdk::UObjectBase* client) {
        initialization.attempted = true;
        const auto reject = [](const char* stage) {
            spdlog::warn("[PROSPI_ROOF] Initialization refused at {}; game visibility is unchanged", stage);
            return false;
        };
        const wchar_t* names[]{
            L"Class /Script/Stadium.StadiumStaticMeshComponent", L"Class /Script/Stadium.StadiumActor",
            L"Class /Script/Engine.StaticMesh", L"Class /Script/Engine.Level", L"Class /Script/Engine.World",
            L"Class /Script/Engine.SceneComponent", L"Class /Script/Engine.GameViewportClient",
            L"Class /Script/Engine.GameEngine"};
        for (size_t i{}; i < classes.size(); ++i) {
            const auto id = find_class(names[i]);
            if (!id) { return reject("native class discovery"); }
            classes[i] = *id;
        }
        component_class = classes[0]; actor_class = classes[1]; mesh_class = classes[2];
        level_class = classes[3]; world_class = classes[4];
        const auto engine_id = observe(sdk::UEngine::get());
        const auto eclass = engine_id && runtime(*engine_id) ? observe(object(*engine_id)->get_class()) : std::nullopt;
        if (!eclass || !static_cast<sdk::UObject*>(object(*engine_id))->is_a(klass(classes[7]))) {
            return reject("active engine identity/class");
        }
        const auto ev = field(klass(*eclass), L"GameViewport", L"ObjectProperty", sizeof(uintptr_t));
        sdk::UObjectBase* viewport_ptr{};
        if (!ev || !read(engine_id->object + *ev, viewport_ptr)) { return reject("Engine.GameViewport field"); }
        const auto client_id = observe(viewport_ptr);
        const auto client_class = client_id && runtime(*client_id) ? observe(viewport_ptr->get_class()) : std::nullopt;
        const auto client_outer = client_id ? outer(*client_id) : std::nullopt;
        if (!client_class || !client_outer || !sdk::object_liveness::matches(*engine_id, *client_outer) ||
            !static_cast<sdk::UObject*>(viewport_ptr)->is_a(klass(classes[6]))) {
            return reject("active viewport identity/class/owner");
        }
        const auto adjustment = viewport_dispatch_offset(reinterpret_cast<uintptr_t>(client), client_id->object);
        uintptr_t table{}, secondary{};
        if (!adjustment || !read(client_id->object, table) || !module_data(table, sizeof(uintptr_t))) {
            return reject("viewport Draw dispatch identity/vtable");
        }
        if (*adjustment) {
            const auto draw = sdk::UGameViewportClient::get_draw_function();
            uintptr_t target{};
            if (!draw || !read(reinterpret_cast<uintptr_t>(client), secondary) ||
                !module_data(secondary, (viewport_interface_draw_slot + 1) * sizeof(uintptr_t)) ||
                !read(secondary + viewport_interface_draw_slot * sizeof(uintptr_t), target) || target != *draw) {
                return reject("secondary viewport Draw vtable");
            }
            draw_function = *draw;
        }
        engine = *engine_id; engine_class = *eclass; engine_viewport_offset = *ev;
        dispatch_offset = *adjustment; viewport_vtable = table; interface_vtable = secondary;
        viewport_class = *client_class;
        if (!current_viewport(client)) { return reject("stable active viewport ownership"); }
        const auto w = field(klass(*client_class), L"World", L"ObjectProperty", 8);
        const auto l = field(klass(world_class), L"Levels", L"ArrayProperty", 16);
        const auto m = field(klass(component_class), L"StaticMesh", L"ObjectProperty", 8);
        const auto v = boolean(klass(component_class), L"bVisible");
        const auto h = boolean(klass(component_class), L"bHiddenInGame");
        const auto a = boolean(klass(actor_class), L"bHidden");
        if (!w || !l || !m || !v || !h || !a) { return reject("reflected world/level/mesh/visibility fields"); }
        const auto lp = static_cast<sdk::FArrayProperty*>(klass(world_class)->find_property(L"Levels"));
        if (!property_type(lp->get_inner(), L"ObjectProperty")) { return reject("World.Levels inner type"); }
        const auto fn = klass(classes[5])->find_function(L"SetVisibility");
        const auto fn_id = observe(fn);
        if (!fn_id || !fn->is_native() || fn->get_num_parms() != 2 || fn->get_parms_size() != 2) {
            return reject("SetVisibility function/parameter layout");
        }
        const auto native_bool = [&](const wchar_t* name, uint32_t offset) {
            const auto f = field(fn, name, L"BoolProperty", 1);
            const auto p = f ? static_cast<sdk::FBoolProperty*>(fn->find_property(name)) : nullptr;
            return p && p->is_param() && !p->is_out_param() && !p->is_return_param() &&
                *f == offset && native_bool_storage(p->get_field_size(), p->get_byte_offset(),
                    p->get_byte_mask(), p->get_field_mask());
        };
        if (!native_bool(L"bNewVisibility", 0) || !native_bool(L"bPropagateToChildren", 1)) {
            return reject("SetVisibility native bool metadata");
        }
        world_offset = *w; levels_offset = *l; mesh_offset = *m;
        visible = *v; hidden = *h; actor_hidden = *a;
        levels.reserve(max_levels);
        levels_scratch.reserve(max_levels);
        entries.reserve(max_targets);
        const auto target = resolve_setter(fn, visible);
        if (!target) { return reject("native setter code/unwind validation"); }
        auto created = safetyhook::InlineHook::create(reinterpret_cast<void*>(*target), &detour,
            safetyhook::InlineHook::StartDisabled);
        if (!created) { return reject("visibility hook creation"); }
        hook = std::move(*created);
        original = hook.original<Native>();
        std::shared_ptr<Impl> empty;
        if (!active.compare_exchange_strong(empty, shared_from_this())) {
            hook.reset(); return reject("visibility hook ownership");
        }
        native_entry.store(reinterpret_cast<Native>(*target), std::memory_order_release);
        if (!hook.enable()) {
            active.store(nullptr, std::memory_order_release);
            hook.reset(); return reject("visibility hook enable");
        }
        initialization.supported = true;
        spdlog::info("[PROSPI_ROOF] Validated SceneComponent visibility setter at 0x{:x}; viewport dispatch +0x{:x}; all-stadium opt-in",
            *target, dispatch_offset);
        return true;
    }

    std::optional<Identity> current_viewport(sdk::UObjectBase* dispatch) const {
        sdk::UObjectBase* before{}, *after{};
        uintptr_t table{}, secondary{}, draw{}, owner{};
        if (!runtime(engine) || !sdk::is_current_object(engine_class) || !has_class(engine, engine_class) ||
            reinterpret_cast<uintptr_t>(sdk::UEngine::get()) != engine.object ||
            !read(engine.object + engine_viewport_offset, before)) { return {}; }
        const auto id = observe(before);
        const auto adjustment = id ? viewport_dispatch_offset(reinterpret_cast<uintptr_t>(dispatch), id->object) : std::nullopt;
        if (!adjustment || *adjustment != dispatch_offset || !runtime(*id) || !has_class(*id, viewport_class) ||
            !read(id->object + sdk::UObjectBase::get_outer_private_offset(), owner) || owner != engine.object ||
            !read(id->object, table) || table != viewport_vtable) { return {}; }
        if (dispatch_offset && (!read(reinterpret_cast<uintptr_t>(dispatch), secondary) || secondary != interface_vtable ||
            !read(secondary + viewport_interface_draw_slot * sizeof(uintptr_t), draw) || draw != draw_function)) { return {}; }
        return read(engine.object + engine_viewport_offset, after) && before == after && sdk::is_current_object(*id) ? id : std::nullopt;
    }

    bool current_world() const {
        sdk::UObjectBase* value{}, *active_viewport{};
        return runtime(engine) && sdk::is_current_object(engine_class) && has_class(engine, engine_class) &&
            reinterpret_cast<uintptr_t>(sdk::UEngine::get()) == engine.object &&
            read(engine.object + engine_viewport_offset, active_viewport) && reinterpret_cast<uintptr_t>(active_viewport) == viewport.object &&
            runtime(viewport) && has_class(viewport, viewport_class) && runtime(world) &&
            read(viewport.object + world_offset, value) && reinterpret_cast<uintptr_t>(value) == world.object;
    }
    bool read_levels(std::vector<uintptr_t>& out) const {
        sdk::TArrayLite<uintptr_t> before{}, after{};
        if (!current_world() || !read(world.object + levels_offset, before) || before.count < 0 ||
            before.count > static_cast<int32_t>(max_levels) || before.capacity < before.count ||
            before.capacity > 4096 || (before.count && !before.data)) { return false; }
        out.resize(before.count);
        if (before.count && !read_bytes(reinterpret_cast<uintptr_t>(before.data), out.data(), out.size() * sizeof(uintptr_t))) { return false; }
        return read(world.object + levels_offset, after) && before.data == after.data &&
            before.count == after.count && before.capacity == after.capacity;
    }
    bool level_in_world(uintptr_t level) const {
        sdk::TArrayLite<uintptr_t> before{}, after{};
        std::array<uintptr_t, max_levels> list{};
        if (!current_world() || !read(world.object + levels_offset, before) || before.count < 0 ||
            before.count > static_cast<int32_t>(list.size()) || before.capacity < before.count ||
            before.capacity > 4096 || (before.count && !before.data) ||
            (before.count && !read_bytes(reinterpret_cast<uintptr_t>(before.data), list.data(), before.count * sizeof(uintptr_t))) ||
            !read(world.object + levels_offset, after) || before.data != after.data || before.count != after.count ||
            before.capacity != after.capacity) { return false; }
        return std::find(list.begin(), list.begin() + before.count, level) != list.begin() + before.count;
    }
    bool eligible(const Entry& entry) const {
        bool owner_hidden{}, component_hidden{};
        uintptr_t owner{}, level{}, mesh{};
        if (!read(entry.component.object + sdk::UObjectBase::get_outer_private_offset(), owner) || owner != entry.owner.object ||
            !read(entry.owner.object + sdk::UObjectBase::get_outer_private_offset(), level) || level != entry.level.object ||
            !read(entry.component.object + mesh_offset, mesh) || mesh != entry.mesh.object ||
            !actor_hidden.get(entry.owner, owner_hidden) || !hidden.get(entry.component, component_hidden)) { return false; }
        // Membership also validates the live viewport/world. Reuse that result
        // within this check, never across game calls or viewport Draws.
        const auto current_scope = level_in_world(entry.level.object);
        return Eligibility{
            true, GetCurrentThreadId() == thread.load(), current_scope, runtime(entry.component),
            runtime(entry.owner), sdk::is_current_object(entry.mesh) && sdk::is_current_object(entry.package),
            runtime(entry.level), current_scope, owner_hidden, component_hidden}.accepted();
    }
    Entry* learn(sdk::UObjectBase* component, std::optional<bool> requested = {}) {
        const auto id = observe(component);
        if (!id || !runtime(*id) || !has_class(*id, component_class)) { return nullptr; }
        bool inherited_override{};
        const auto existing = entries.find(id->object);
        if (existing != entries.end()) {
            if (sdk::object_liveness::matches(existing->second.component, *id) && eligible(existing->second)) {
                return &existing->second;
            }
            // A different mesh/owner on the same component cannot learn from
            // a visible value that this guard forced for the previous binding.
            inherited_override = sdk::object_liveness::matches(existing->second.component, *id) &&
                (existing->second.lease.forced || existing->second.proof.inherited_override);
            entries.erase(existing);
        }
        const auto owner = outer(*id);
        if (!owner || !runtime(*owner) || !has_class(*owner, actor_class)) { return nullptr; }
        const auto level = outer(*owner);
        if (!level || !runtime(*level) || !has_class(*level, level_class) || !level_in_world(level->object)) { return nullptr; }
        sdk::UObjectBase* mesh_ptr{};
        if (!read(id->object + mesh_offset, mesh_ptr)) { return nullptr; }
        const auto mesh = observe(mesh_ptr);
        if (!mesh || !has_class(*mesh, mesh_class)) { return nullptr; }
        const auto package = outer(*mesh);
        if (!package) { return nullptr; }
        const auto kind = geometry(component->get_name_safe(), mesh_ptr->get_name_safe(), object(*package)->get_name_safe());
        if (kind == Geometry::rejected) { return nullptr; }
        bool current{};
        Entry entry{*id, *owner, *level, *mesh, *package};
        if (!eligible(entry) || !visible.get(*id, current)) { return nullptr; }
        entry.lease.game_request(current);
        entry.proof.kind = kind;
        entry.proof.inherited_override = inherited_override;
        if (requested.has_value()) { entry.proof.game_request(*requested, current); }
        else { entry.proof.sample_unforced(current); }
        if (entries.size() >= max_targets) {
            // Speculative background discovery must not crowd out the previous
            // structural path. Never evict a restoration obligation or proof.
            if (kind != Geometry::structural) { return nullptr; }
            const auto pending = std::find_if(entries.begin(), entries.end(), [](const auto& item) {
                return disposable_pending(item.second.proof, item.second.lease);
            });
            if (pending == entries.end()) { return nullptr; }
            entries.erase(pending);
        }
        return &entries.emplace(id->object, std::move(entry)).first->second;
    }
    bool force(Entry& entry, std::optional<bool> requested = {}) {
        bool current{};
        if (!eligible(entry) || !visible.get(entry.component, current)) { return false; }
        if (!entry.proof.allows_override()) {
            // New background groups must first be seen game-visible. A pending
            // entry has never been forced, so our own readback cannot train it.
            if (requested.has_value()) { entry.proof.game_request(*requested, current); }
            else { entry.proof.sample_unforced(current); }
            if (!entry.proof.allows_override()) { return true; }
        }
        if (!current) {
            entry.lease.game_request(false);
            const auto binding = entry;
            // NoPropagation: do not unhide unrelated attached meshes, masks or
            // LODs. OnVisibilityChanged still performs normal render-state work.
            original(object(binding.component), true, uint8_t{0});
            // A callback may alter the cache. Reacquire instead of retaining a
            // borrowed entry across the native call or updating a reused object.
            const auto cached = entries.find(binding.component.object);
            if (cached != entries.end() && sdk::object_liveness::matches(cached->second.component, binding.component) &&
                sdk::object_liveness::matches(cached->second.owner, binding.owner) &&
                sdk::object_liveness::matches(cached->second.level, binding.level) &&
                sdk::object_liveness::matches(cached->second.mesh, binding.mesh) &&
                sdk::object_liveness::matches(cached->second.package, binding.package) &&
                eligible(cached->second) && visible.get(binding.component, current)) {
                cached->second.lease.override_applied(current);
                if (current) { ++override_count; }
            }
        }
        return true;
    }
    void game_request(sdk::UObjectBase* component, bool requested) {
        // Non-stadium scene components never incur names, scans or mutations.
        uintptr_t cls{};
        if (!read(reinterpret_cast<uintptr_t>(component) + sdk::UObjectBase::get_class_private_offset(), cls) ||
            cls != component_class.object) { return; }
        if (auto entry = learn(component, requested)) {
            entry->lease.game_request(requested);
            force(*entry, requested);
        }
    }
    void release() {
        enabled.store(false, std::memory_order_release);
        for (auto& [_, entry] : entries) {
            bool current{};
            if (eligible(entry) && visible.get(entry.component, current) && entry.lease.should_restore(current)) {
                original(object(entry.component), entry.lease.requested, uint8_t{0});
            }
        }
        entries.clear(); levels.clear(); maintenance_cursor = 0; scan_cursor = 0; next_scan = {};
    }
    void update(sdk::UObjectBase* client, bool requested) {
        thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        if (!requested) {
            if (!entries.empty() || enabled.load()) { release(); }
            initialization.on_disabled(static_cast<bool>(hook));
            return;
        }
        if (!initialization.attempted && !initialize(client)) { return; }
        if (!initialization.supported) { return; }
        if (!sdk::is_current_object(viewport_class) ||
            std::any_of(classes.begin(), classes.end(), [](const Identity& id) { return !sdk::is_current_object(id); })) {
            enabled.store(false, std::memory_order_release); entries.clear(); initialization.supported = false; return;
        }
        const auto client_id = current_viewport(client);
        sdk::UObjectBase* world_ptr{};
        const auto w = client_id && runtime(*client_id) && has_class(*client_id, viewport_class) &&
            read(client_id->object + world_offset, world_ptr) ? observe(world_ptr) : std::nullopt;
        if (!w || !runtime(*w) || !has_class(*w, world_class)) {
            enabled.store(false, std::memory_order_release); entries.clear(); world = {}; viewport = {}; return;
        }
        if (!sdk::object_liveness::matches(world, *w) || !sdk::object_liveness::matches(viewport, *client_id)) {
            // Never restore/write into a previous or unloading world.
            entries.clear(); levels.clear(); maintenance_cursor = 0; scan_cursor = 0; next_scan = {};
            world = *w; viewport = *client_id;
        }
        if (!read_levels(levels_scratch)) { enabled.store(false); return; }
        if (levels_scratch != levels) { levels.swap(levels_scratch); scan_cursor = 0; next_scan = {}; }
        enabled.store(true, std::memory_order_release);
        const auto now = Clock::now();
        const auto deadline = now + std::chrono::microseconds{250};
        std::array<uintptr_t, max_targets> keys{};
        size_t key_count{};
        for (const auto& [key, _] : entries) { keys[key_count++] = key; }
        for (size_t n{}; n < (std::min)(key_count, cached_targets_per_draw) && Clock::now() < deadline; ++n) {
            const auto key = keys[maintenance_index(maintenance_cursor, key_count)];
            if (const auto it = entries.find(key); it != entries.end() && !force(it->second)) { entries.erase(key); }
        }
        if (now < next_scan) { return; }
        const auto array = sdk::FUObjectArray::get();
        const auto count = array ? array->get_object_count() : 0;
        if (count <= 0 || count > 500000) { return; }
        if (scan_cursor <= 0 || scan_cursor > count) { scan_cursor = count; }
        const auto end = (std::max)(0, scan_cursor - scan_objects_per_draw);
        for (; scan_cursor > end && Clock::now() < deadline; --scan_cursor) {
            const auto item = array->get_object(scan_cursor - 1);
            const auto component = item ? item->get_object() : nullptr;
            uintptr_t cls{};
            if (component && read(reinterpret_cast<uintptr_t>(component) + sdk::UObjectBase::get_class_private_offset(), cls) &&
                cls == component_class.object) {
                if (auto entry = learn(component)) { force(*entry); }
            }
        }
        if (scan_cursor == 0) { next_scan = now + std::chrono::seconds{5}; }
    }
    void retire() {
        enabled.store(false, std::memory_order_release);
        // A failed unpatch must keep the pass-through owner/trampoline alive.
        if (hook && !hook.disable()) { return; }
        auto expected = shared_from_this();
        active.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        // The last in-flight detour retains this owner and its trampoline.
    }
};

VisibilityGuard::~VisibilityGuard() { if (m_impl) { m_impl->retire(); } }
void VisibilityGuard::update(sdk::UObjectBase* viewport, bool enabled) noexcept {
    if (!enabled && !m_impl) { return; }
    if (!sdk::UObjectBase::has_validated_header_layout()) { m_status.store(Status::waiting); return; }
    try {
        if (!m_impl) { m_impl = std::make_shared<Impl>(); }
        const auto previous = status();
        m_impl->update(viewport, enabled);
        const auto learning = static_cast<uint32_t>(std::count_if(m_impl->entries.begin(), m_impl->entries.end(),
            [](const auto& item) { return !item.second.proof.allows_override(); }));
        const auto targets = static_cast<uint32_t>(m_impl->entries.size()) - learning;
        const auto current = !enabled ? Status::off :
            m_impl->initialization.attempted && !m_impl->initialization.supported ? Status::unsupported :
            m_impl->enabled.load() && targets ? Status::active :
            m_impl->enabled.load() && learning ? Status::learning : Status::waiting;
        m_status.store(current, std::memory_order_relaxed);
        m_count.store(targets, std::memory_order_relaxed);
        m_learning.store(learning, std::memory_order_relaxed);
        m_overrides.store(m_impl->override_count, std::memory_order_relaxed);
        if (previous != current) {
            spdlog::info("[PROSPI_ROOF] state={} targets={} learning={} overrides={}", status_name(current), count(), learning_count(), overrides());
        }
    } catch (...) {
        if (m_impl) { m_impl->enabled.store(false, std::memory_order_release); }
        m_status.store(Status::unsupported, std::memory_order_relaxed);
    }
}
}
