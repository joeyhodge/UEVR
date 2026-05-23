#include <atomic>
#include <algorithm>
#include <fstream>
#include <vector>

#include <utility/Logging.hpp>
#include <utility/Module.hpp>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/UObjectBase.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FField.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/FEnumProperty.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/AActor.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/FMalloc.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/USceneComponent.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/APawn.hpp>
#include <sdk/APlayerCameraManager.hpp>
#include <sdk/ScriptVector.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/FObjectProperty.hpp>
#include <sdk/FArrayProperty.hpp>
#include <sdk/UMotionControllerComponent.hpp>
#include <sdk/Utility.hpp>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "uobjecthook/SDKDumper.hpp"
#include "VR.hpp"

#include "GameSpecific.hpp"
#include "UObjectHook.hpp"

//#define VERBOSE_UOBJECTHOOK

std::shared_ptr<UObjectHook>& UObjectHook::get() {
    static std::shared_ptr<UObjectHook> instance = std::make_shared<UObjectHook>();
    return instance;
}

namespace {
constexpr size_t STALKER2_BULK_SCENE_ATTACHMENT_CAP = 32;
constexpr uint32_t STALKER2_FULL_SCAN_BUDGET = 128;
constexpr size_t STALKER2_PERSISTENT_STATE_RESOLVE_BUDGET = 8;
constexpr size_t STALKER2_PERSISTENT_PROPERTY_RESOLVE_BUDGET = 4;
constexpr size_t STALKER2_CLASS_BROWSER_CLASS_CAP = 256;
constexpr size_t STALKER2_CLASS_BROWSER_OBJECT_CAP = 128;

bool is_ue_5_1_uobjecthook_guard_enabled() {
    static const bool is_ue_5_1 = []() {
        const auto disk_version = sdk::get_file_version_info();
        const auto found_version = sdk::search_for_version(GetModuleHandleW(nullptr));

        if (found_version) {
            const auto version = utility::narrow(*found_version);
            return version == "5.1" || version.starts_with("5.1.");
        }

        return disk_version.dwFileVersionMS == 0x00050001;
    }();

    return is_ue_5_1;
}

bool is_avowed_uobjecthook_guard_enabled() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_avowed_executable_path(*exe_path);
    }();

    return result;
}

bool is_stalker2_uobjecthook_guard_enabled() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Stalker2-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool is_everwind_uobjecthook_guard_enabled() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Everwind-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool use_dynamic_uobjecthook_candidate_guard() {
    return is_ue_5_1_uobjecthook_guard_enabled() ||
        is_avowed_uobjecthook_guard_enabled() ||
        is_stalker2_uobjecthook_guard_enabled() ||
        is_everwind_uobjecthook_guard_enabled();
}

bool should_incrementally_refresh_uobject_array() {
    return is_ue_5_1_uobjecthook_guard_enabled() ||
        is_stalker2_uobjecthook_guard_enabled();
}

bool should_use_stalker2_on_demand_uobject_tracking(int32_t object_count) {
    (void)object_count;
    return is_stalker2_uobjecthook_guard_enabled();
}

bool should_use_everwind_lazy_uobjecthook_startup() {
    return is_everwind_uobjecthook_guard_enabled();
}

bool is_stalker2_bulk_scene_component(sdk::USceneComponent* component) {
    if (!is_stalker2_uobjecthook_guard_enabled() || component == nullptr) {
        return false;
    }

    if (!sdk::UObjectReference{component}.valid()) {
        return false;
    }

    const auto component_class = component->get_class();

    if (component_class == nullptr) {
        return false;
    }

    static const auto scene_component_class = sdk::USceneComponent::static_class();

    if (component_class != scene_component_class) {
        return false;
    }

    const auto component_name = component->get_fname().to_string();
    return component_name.rfind(L"SceneComponent", 0) == 0;
}

bool is_readable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};

    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    const auto readable =
        protect == PAGE_READONLY ||
        protect == PAGE_READWRITE ||
        protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READ ||
        protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;

    if (!readable) {
        return false;
    }

    const auto region_start = (uintptr_t)mbi.BaseAddress;
    const auto region_end = region_start + mbi.RegionSize;

    return address >= region_start && address <= region_end && size <= (region_end - address);
}

bool safe_read_uintptr(uintptr_t address, uintptr_t& value) {
    if (!is_readable_process_range(address, sizeof(uintptr_t))) {
        return false;
    }

    __try {
        value = *(uintptr_t*)address;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

bool safe_read_u32(uintptr_t address, uint32_t& value) {
    if (!is_readable_process_range(address, sizeof(uint32_t))) {
        return false;
    }

    __try {
        value = *(uint32_t*)address;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

bool has_plausible_fname(uintptr_t object_address) {
    if (!is_readable_process_range(object_address + sdk::UObjectBase::get_fname_offset(), sizeof(sdk::FName))) {
        return false;
    }

    uint32_t comparison_index{};

    if (!safe_read_u32(object_address + sdk::UObjectBase::get_fname_offset(), comparison_index)) {
        return false;
    }

    const auto block_index = comparison_index >> 16;

    // Keep this intentionally loose; the goal is to reject obvious non-UObjects without
    // depending on a specific game's name-pool limits during very early object startup.
    return block_index < 0x4000;
}

bool is_uobject_array_member(sdk::UObjectBase* object) {
    const auto object_address = (uintptr_t)object;

    if (object_address == 0) {
        return false;
    }

    uint32_t internal_index{};

    if (!safe_read_u32(object_address + sdk::UObjectBase::get_internal_index_offset(), internal_index)) {
        return false;
    }

    auto object_array = sdk::FUObjectArray::get();

    if (object_array == nullptr) {
        return false;
    }

    const auto object_count = object_array->get_object_count();

    if (object_count <= 0 || internal_index >= (uint32_t)object_count) {
        return false;
    }

    auto item = object_array->get_object((int32_t)internal_index);

    if (item == nullptr || !is_readable_process_range((uintptr_t)item, sizeof(sdk::FUObjectItem))) {
        return false;
    }

    uintptr_t item_object{};

    if (!safe_read_uintptr((uintptr_t)item + sdk::FUObjectArray::get_item_object_offset(), item_object)) {
        return false;
    }

    return item_object == object_address;
}

std::optional<std::pair<int32_t, int32_t>> get_uobject_index_serial(sdk::UObjectBase* object) {
    const auto object_address = (uintptr_t)object;

    if (object_address == 0) {
        return std::nullopt;
    }

    uint32_t internal_index{};

    if (!safe_read_u32(object_address + sdk::UObjectBase::get_internal_index_offset(), internal_index)) {
        return std::nullopt;
    }

    auto object_array = sdk::FUObjectArray::get();

    if (object_array == nullptr) {
        return std::nullopt;
    }

    const auto object_count = object_array->get_object_count();

    if (object_count <= 0 || internal_index >= (uint32_t)object_count) {
        return std::nullopt;
    }

    auto item = object_array->get_object((int32_t)internal_index);

    if (item == nullptr || !is_readable_process_range((uintptr_t)item, sizeof(sdk::FUObjectItem))) {
        return std::nullopt;
    }

    uintptr_t item_object{};

    if (!safe_read_uintptr((uintptr_t)item + sdk::FUObjectArray::get_item_object_offset(), item_object) || item_object != object_address) {
        return std::nullopt;
    }

    return std::pair{(int32_t)internal_index, item->get_serial_number()};
}

bool is_probably_uobject_layout(sdk::UObjectBase* object, uintptr_t* class_address = nullptr) {
    const auto object_address = (uintptr_t)object;

    if (!is_readable_process_range(object_address, sdk::UObjectBase::get_class_size())) {
        return false;
    }

    uintptr_t object_vtable{};

    if (!safe_read_uintptr(object_address, object_vtable) || !is_readable_process_range(object_vtable, sizeof(uintptr_t))) {
        return false;
    }

    uintptr_t class_ptr{};

    if (!safe_read_uintptr(object_address + sdk::UObjectBase::get_class_private_offset(), class_ptr) || class_ptr == 0) {
        return false;
    }

    if (!is_readable_process_range(class_ptr, sdk::UObjectBase::get_class_size())) {
        return false;
    }

    uintptr_t class_vtable{};

    if (!safe_read_uintptr(class_ptr, class_vtable) || !is_readable_process_range(class_vtable, sizeof(uintptr_t))) {
        return false;
    }

    uintptr_t class_class_ptr{};

    if (!safe_read_uintptr(class_ptr + sdk::UObjectBase::get_class_private_offset(), class_class_ptr) || class_class_ptr == 0) {
        return false;
    }

    if (!is_readable_process_range(class_class_ptr, sizeof(uintptr_t))) {
        return false;
    }

    if (!has_plausible_fname(object_address) || !has_plausible_fname(class_ptr)) {
        return false;
    }

    if (class_address != nullptr) {
        *class_address = class_ptr;
    }

    return true;
}

bool is_safe_uobject_candidate(UObjectHook& hook, sdk::UObjectBase* object, bool require_array_member = false) {
    uintptr_t class_address{};

    if (!is_probably_uobject_layout(object, &class_address)) {
        return false;
    }

    if (require_array_member && !is_uobject_array_member(object)) {
        return false;
    }

    const auto cls = (sdk::UObjectBase*)class_address;

    if (hook.exists(cls)) {
        return true;
    }

    return is_probably_uobject_layout(cls);
}
}

UObjectHook::MotionControllerState::~MotionControllerState() {
    if (this->adjustment_visualizer != nullptr) {
        GameThreadWorker::get().enqueue([vis = this->adjustment_visualizer]() {
            SPDLOG_INFO("[UObjectHook::MotionControllerState] Destroying adjustment visualizer for component {:x}", (uintptr_t)vis);

            if (!UObjectHook::get()->exists(vis)) {
                return;
            }

            vis->destroy_actor();

            SPDLOG_INFO("[UObjectHook::MotionControllerState] Destroyed adjustment visualizer for component {:x}", (uintptr_t)vis);
        });
    }
}

nlohmann::json UObjectHook::MotionControllerStateBase::to_json() const {
    return {
        {"rotation_offset", utility::math::to_json(rotation_offset)},
        {"location_offset", utility::math::to_json(location_offset)},
        {"hand", hand},
        {"permanent", permanent}
    };
}

void UObjectHook::MotionControllerStateBase::from_json(const nlohmann::json& data) {
    if (data.contains("rotation_offset")) {
        rotation_offset = utility::math::from_json_quat(data["rotation_offset"]);
    }

    if (data.contains("location_offset")) {
        location_offset = utility::math::from_json_vec3(data["location_offset"]);
    }

    if (data.contains("hand")) {
        hand = data["hand"].get<uint8_t>();
        hand = hand % (uint8_t)MotionControllerStateBase::Hand::LAST;
    }

    if (data.contains("permanent") && data["permanent"].is_boolean()) {
        permanent = data["permanent"].get<bool>();
    }
}

void UObjectHook::activate() {
    if (m_hooked) {
        return;
    }

    if (GameThreadWorker::get().is_same_thread()) {
        hook();
        return;
    }

    m_wants_activate = true;
}

void UObjectHook::hook() {
    if (m_hooked) {
        return;
    }

    SPDLOG_INFO("[UObjectHook] Hooking UObjectBase");

    if (should_use_everwind_lazy_uobjecthook_startup()) {
        // Everwind's shipped UE5.5 build can stall inside UObjectBase AddObject/
        // destructor resolver scans when the UObject/Developer UI is opened.
        // Keep the UI responsive by avoiding the resolver and old-object backfill.
        auto uobjectarray = sdk::FUObjectArray::get();
        const auto object_count = uobjectarray != nullptr ? uobjectarray->get_object_count() : 0;

        m_hooked = true;
        m_wants_activate = false;
        m_add_object_hooked = false;
        m_uobject_array_scan_cursor = object_count;
        m_uobject_array_last_object_count = object_count;
        m_uobject_array_startup_scan_until = std::chrono::steady_clock::now();
        m_uobject_array_full_sweep_active = false;
        m_force_uobject_array_creation_scan.store(false, std::memory_order_relaxed);

        SPDLOG_WARN(
            "[Everwind][UObjectHook] Skipping UObjectBase hook resolver/backfill for {} existing UObjects; using fail-closed lazy UI mode",
            object_count);
        SPDLOG_INFO("[UObjectHook] Deserializing persistent states");
        reload_persistent_states();
        SPDLOG_INFO("[UObjectHook] Deserialized {} persistent states", m_persistent_states.size());

        m_fully_hooked = true;
        return;
    }

    m_hooked = true;
    m_wants_activate = false;

    auto destructor_fn = sdk::UObjectBase::get_destructor();

    if (!destructor_fn) {
        SPDLOG_ERROR("[UObjectHook] Failed to find UObjectBase::destructor, cannot hook UObjectBase");
        return;
    }

    auto add_object_fn = sdk::UObjectBase::get_add_object();

    if (!add_object_fn) {
        SPDLOG_WARN("[UObjectHook] UObjectBase::AddObject was not found; using incremental FUObjectArray creation tracking");
    }

    m_destructor_hook = safetyhook::create_inline((void**)destructor_fn.value(), &destructor);

    if (!m_destructor_hook) {
        SPDLOG_ERROR("[UObjectHook] Failed to hook UObjectBase::destructor, cannot hook UObjectBase");
        return;
    }

    if (add_object_fn) {
        m_add_object_hook = safetyhook::create_inline((void**)add_object_fn.value(), &add_object);

        if (m_add_object_hook) {
            m_add_object_hooked = true;
        } else {
            SPDLOG_WARN("[UObjectHook] Failed to hook UObjectBase::AddObject; using incremental FUObjectArray creation tracking");
        }
    }

    m_force_uobject_array_creation_scan.store(!m_add_object_hooked, std::memory_order_relaxed);

    if (m_add_object_hooked) {
        SPDLOG_INFO("[UObjectHook] Hooked UObjectBase");
    } else {
        SPDLOG_INFO("[UObjectHook] Hooked UObjectBase destructor; AddObject creation tracking is using FUObjectArray scanning");
    }

    // Add all the objects that already exist
    auto uobjectarray = sdk::FUObjectArray::get();
    const auto object_count = uobjectarray != nullptr ? uobjectarray->get_object_count() : 0;

    if (should_use_stalker2_on_demand_uobject_tracking(object_count)) {
        // Stalker2 has a very large live object array. Synchronously adopting it
        // when the UObject UI is first opened causes a visible multi-second hitch.
        // Start at the current end and adopt gameplay roots directly/on demand instead
        // of backfilling hundreds of thousands of old entries on the game thread.
        m_uobject_array_scan_cursor = object_count;
        m_uobject_array_last_object_count = object_count;
        m_uobject_array_startup_scan_until = std::chrono::steady_clock::now();
        m_uobject_array_full_sweep_active = false;
        m_force_uobject_array_creation_scan.store(false, std::memory_order_relaxed);
        SPDLOG_WARN(
            "[Stalker2][UObjectHook] Deferring initial adoption of {} existing UObjects; using on-demand common-root tracking",
            object_count);
    } else {
        for (auto i = 0; i < object_count; ++i) {
            auto object = uobjectarray->get_object(i);

            if (object == nullptr || object->get_object() == nullptr) {
                continue;
            }

            add_new_object(object->get_object());
        }

        m_uobject_array_scan_cursor = object_count;
        m_uobject_array_last_object_count = object_count;
        m_uobject_array_startup_scan_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }

    SPDLOG_INFO("[UObjectHook] Added {} existing objects", m_objects.size());

    SPDLOG_INFO("[UObjectHook] Deserializing persistent states");
    reload_persistent_states();
    SPDLOG_INFO("[UObjectHook] Deserialized {} persistent states", m_persistent_states.size());

    m_fully_hooked = true;
}

void UObjectHook::hook_process_event() {
    if (m_attempted_hook_process_event) {
        return;
    }

    m_attempted_hook_process_event = true;

    auto uobjectarray = sdk::FUObjectArray::get();

    if (uobjectarray == nullptr) {
        return;
    }

    const auto process_event_index = sdk::UObject::get_process_event_index();

    if (process_event_index == 0) {
        return;
    }

    sdk::UObject* first_obj = nullptr;

    for (auto i = 0; i < uobjectarray->get_object_count(); ++i) {
        const auto object = uobjectarray->get_object(i);

        if (object != nullptr && object->get_object() != nullptr) {
            first_obj = (sdk::UObject*)object->get_object();
            break;
        }
    }

    if (first_obj != nullptr) {
        const auto vt = *(void***)first_obj;

        if (vt != nullptr) {
            std::scoped_lock _{m_function_mutex};
            auto fn = vt[process_event_index];
            SPDLOG_INFO("[UObjectHook] ProcessEvent {:x}", (uintptr_t)fn);
            m_process_event_hook = safetyhook::create_inline(fn, &process_event_hook);

            if (m_process_event_hook) {
                SPDLOG_INFO("[UObjectHook] Hooked UObject::ProcessEvent");
                m_hooked_process_event = true;
            } else {
                SPDLOG_ERROR("[UObjectHook] Failed to hook UObject::ProcessEvent");
            }
        }
    }
}

void* UObjectHook::process_event_hook(sdk::UObject* obj, sdk::UFunction* func, void* params, void* r9) {
    auto& hook = UObjectHook::get();

    bool do_heavy_data_once = false;

    if (hook->m_process_event_listening) {
        std::scoped_lock _{hook->m_function_mutex};
        
        auto& data = hook->m_called_functions[func];
        ++data.call_count;

        hook->m_most_recent_functions.push_front(func);

        if (hook->m_most_recent_functions.size() > 200) {
            hook->m_most_recent_functions.pop_back();
        }
    }

    auto result = hook->m_process_event_hook.unsafe_call<void*>(obj, func, params, r9);

    if (hook->m_process_event_listening) {
        std::scoped_lock _{hook->m_function_mutex};

        auto& data = hook->m_called_functions[func];

        if (data.heavy_data == nullptr) {
            do_heavy_data_once = true;
            data.heavy_data = std::make_unique<CalledFunctionInfo::HeavyData>();

            const auto ps = func->get_properties_size();
            const auto ma = func->get_min_alignment();
        
            if (ma > 1) {
                data.heavy_data->params.resize(((ps + ma - 1) / ma) * ma);
            } else {
                data.heavy_data->params.resize(ps);
            }

            memset(data.heavy_data->params.data(), 0, data.heavy_data->params.size());
        }

        if ((data.wants_heavy_data || do_heavy_data_once) && !data.heavy_data->params.empty()) {
            auto bak = data.heavy_data->params;

            try {
                memcpy(data.heavy_data->params.data(), params, data.heavy_data->params.size());
            } catch(...) {
                //SPDLOG_ERROR("[UObjectHook] Failed to copy params for function {:x}", (uintptr_t)func);
                return result;
            }
    
            // Go through all properties and look for arrays and upgrade them so we actually own the data
            // We'll do this by copying the data to a new array
            for (auto prop = func->get_child_properties(); prop != nullptr; prop = prop->get_next()) {
                const auto c = prop->get_class();
                if (c == nullptr) {
                    continue;
                }
    
                const auto cname = c->get_name().to_string();
                const auto cname_hash = ::utility::hash(cname);
    
                switch (cname_hash) {
                case L"ArrayProperty"_fnv:
                {
                    using GenericArray = sdk::TArray<void*>;
                    const auto prop_desc = (sdk::FArrayProperty*)prop;

                    size_t inner_size = sizeof(void*);
                    bool supported = false;
    
                    const auto inner = prop_desc->get_inner();
    
                    if (inner != nullptr) {
                        const auto inner_c = inner->get_class();
    
                        if (inner_c != nullptr) {
                            const auto inner_cname = inner_c->get_name().to_string();
                            const auto inner_cname_hash = ::utility::hash(inner_cname);
    
                            // todo... recursive array stuff? good enough for now
                            switch (inner_cname_hash) {
                            case L"StructProperty"_fnv:
                            {
                                const auto s = ((sdk::FStructProperty*)inner)->get_struct();
    
                                if (s == nullptr) {
                                    break;
                                }
    
                                if (s->is_a(sdk::UScriptStruct::static_class())) {
                                    inner_size = s->get_struct_size();
                                } else {
                                    inner_size = s->get_properties_size();
                                }

                                supported = true;

                                break;
                            }
                            case L"ObjectProperty"_fnv:
                            case L"InterfaceProperty"_fnv:
                            {
                                inner_size = sizeof(void*);
                                supported = true;
                                break;
                            }
    
                            default:
                                break;
                            }
                        }
                    }

                    // uh oh
                    if (prop_desc->get_offset() >= data.heavy_data->params.size()) {
                        break;
                    }

                    auto& arr = *(GenericArray*)((uintptr_t)data.heavy_data->params.data() + prop_desc->get_offset());

                    if (!supported) {
                        arr.count = 0;
                        arr.capacity = 0;
                        arr.data = nullptr;
                        break;
                    }
    
                    auto& bak_arr = *(GenericArray*)((uintptr_t)bak.data() + prop_desc->get_offset());
                    auto new_arr = sdk::TArray<void*>{};
                    auto fmalloc = sdk::FMalloc::get();

                    if (bak_arr.data != nullptr) {
                        new_arr = std::move(bak_arr);
    
                        if (arr.capacity > new_arr.capacity) {
                            if (fmalloc != nullptr) {
                                new_arr.data = (void**)fmalloc->realloc(new_arr.data, arr.capacity * inner_size, sizeof(void*));
                                new_arr.capacity = arr.capacity;
                            } else {
                                static bool logged_missing_fmalloc = false;

                                if (!logged_missing_fmalloc) {
                                    logged_missing_fmalloc = true;
                                    SPDLOG_WARN("[UObjectHook] FMalloc is unavailable; capping ProcessEvent array parameter copies to existing storage");
                                }
                            }
                        }
    
                        new_arr.count = std::min(arr.count, new_arr.capacity);
                    } else if (arr.capacity > 0) {
                        if (fmalloc != nullptr) {
                            new_arr.data = (void**)fmalloc->malloc(arr.capacity * inner_size, sizeof(void*));
                            std::memset(new_arr.data, 0, arr.capacity * inner_size);
                            new_arr.count = arr.count;
                            new_arr.capacity = arr.capacity;
                        } else {
                            static bool logged_missing_fmalloc = false;

                            if (!logged_missing_fmalloc) {
                                logged_missing_fmalloc = true;
                                SPDLOG_WARN("[UObjectHook] FMalloc is unavailable; skipping ProcessEvent array parameter allocation");
                            }
                        }
                    }
    
                    if (arr.data != nullptr && new_arr.count > 0 && arr.capacity >= new_arr.count && new_arr.data != nullptr && !IsBadReadPtr((void*)arr.data, new_arr.count * inner_size)) {
                        memcpy(new_arr.data, arr.data, new_arr.count * inner_size);
                        arr.data = nullptr;
                        arr.count = 0;
                        arr.capacity = 0;
                        arr = std::move(new_arr);
                    } else {
                        arr.count = 0;  
                    }
                }
                case L"StrProperty"_fnv:
                {
                    using FString = sdk::TArray<wchar_t>;
    
                    const auto prop_desc = (sdk::FProperty*)prop;

                    // uh oh
                    if (prop_desc->get_offset() >= data.heavy_data->params.size()) {
                        break;
                    }

                    auto& str = *(FString*)((uintptr_t)data.heavy_data->params.data() + prop_desc->get_offset());
                    auto& bak_str = *(FString*)((uintptr_t)bak.data() + prop_desc->get_offset());
    
                    auto new_str = FString{};
                    auto fmalloc = sdk::FMalloc::get();
    
                    // Same thing but much simpler because we know it's wchar_t
                    if (bak_str.data != nullptr) {
                        new_str = std::move(bak_str);
    
                        if (str.capacity > new_str.capacity) {
                            if (fmalloc != nullptr) {
                                new_str.data = (wchar_t*)fmalloc->realloc(new_str.data, str.capacity * sizeof(wchar_t), sizeof(wchar_t));
                                new_str.capacity = str.capacity;
                            } else {
                                static bool logged_missing_fmalloc = false;

                                if (!logged_missing_fmalloc) {
                                    logged_missing_fmalloc = true;
                                    SPDLOG_WARN("[UObjectHook] FMalloc is unavailable; capping ProcessEvent string parameter copies to existing storage");
                                }
                            }
                        }
    
                        new_str.count = std::min(str.count, new_str.capacity);
                    } else if (str.capacity > 0) {
                        if (fmalloc != nullptr) {
                            new_str.data = (wchar_t*)fmalloc->malloc(str.capacity * sizeof(wchar_t), sizeof(wchar_t));
                            std::memset(new_str.data, 0, str.capacity * sizeof(wchar_t));
                            new_str.count = str.count;
                            new_str.capacity = str.capacity;
                        } else {
                            static bool logged_missing_fmalloc = false;

                            if (!logged_missing_fmalloc) {
                                logged_missing_fmalloc = true;
                                SPDLOG_WARN("[UObjectHook] FMalloc is unavailable; skipping ProcessEvent string parameter allocation");
                            }
                        }
                    }
    
                    if (str.data != nullptr && new_str.count > 0 && str.capacity >= new_str.count && new_str.data != nullptr && !IsBadReadPtr((void*)str.data, new_str.count * sizeof(wchar_t))) {
                        memcpy(new_str.data, str.data, new_str.count * sizeof(wchar_t));
                        str.data = nullptr;
                        str.count = 0;
                        str.capacity = 0;
                        str = std::move(new_str);
                    } else {
                        str.count = 0;
                    }
                }
                    break;
                default:
                    break;
                };
            }
        }
    }

    return result;
}

void UObjectHook::add_new_object(sdk::UObjectBase* object, bool run_creation_jobs) {
    {
        std::shared_lock _{m_mutex};
        if (exists_unsafe(object)) {
            return;
        }
    }

    const auto require_array_member = is_avowed_uobjecthook_guard_enabled();

    if (use_dynamic_uobjecthook_candidate_guard() && !is_safe_uobject_candidate(*this, object, require_array_member)) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[UObjectHook] Skipping unsafe UObject candidate {:x}", (uintptr_t)object);
        return;
    }

    std::unique_lock _{m_mutex};

    if (exists_unsafe(object)) {
        return;
    }

    std::unique_ptr<MetaObject> meta_object{};

    /*static const auto prim_comp_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.PrimitiveComponent");

    if (prim_comp_t != nullptr && object->get_class()->is_a(prim_comp_t)) {
        static const auto bGenerateOverlapEvents = (sdk::FBoolProperty*)prim_comp_t->find_property(L"bGenerateOverlapEvents");

        if (bGenerateOverlapEvents != nullptr) {
            bGenerateOverlapEvents->set_value_in_object(object, true);
        }
    }*/

    const auto c = object->get_class();

    if (c == nullptr) {
        return;
    }

    if (!m_reusable_meta_objects.empty()) {
        meta_object = std::move(m_reusable_meta_objects.back());
        m_reusable_meta_objects.pop_back();
    } else {
        meta_object = std::make_unique<MetaObject>();
        meta_object->super_classes.reserve(16);
    }

    m_objects.insert(object);
    meta_object->super_classes.clear();
    meta_object->full_name = object->get_full_name();
    meta_object->uclass = object->get_class();

    m_most_recent_objects.push_front((sdk::UObject*)object);

    if (m_most_recent_objects.size() > 50) {
        m_most_recent_objects.pop_back();
    }

    for (auto super = (sdk::UStruct*)object->get_class(); super != nullptr; super = super->get_super_struct()) {
        meta_object->super_classes.push_back((sdk::UClass*)super);

        m_objects_by_class[(sdk::UClass*)super].insert(object);

        if (run_creation_jobs && m_on_creation_add_component_jobs.contains((sdk::UClass*)super)) {
            GameThreadWorker::get().enqueue([object, this]() {
                if (!this->exists(object)) {
                    return;
                }

                for (auto super = (sdk::UStruct*)object->get_class(); super != nullptr; super = super->get_super_struct()) {
                    std::function<void(sdk::UObject*)> job{};

                    {
                        std::shared_lock _{m_mutex};

                        if (auto it = this->m_on_creation_add_component_jobs.find((sdk::UClass*)super); it != this->m_on_creation_add_component_jobs.end()) {
                            job = it->second;
                        }
                    }

                    if (job) {
                        job((sdk::UObject*)object);
                    }
                }
            });
        }
    }

    m_meta_objects[object] = std::move(meta_object);

#ifdef VERBOSE_UOBJECTHOOK
    SPDLOG_INFO("Adding object {:x} {:s}", (uintptr_t)object, utility::narrow(m_meta_objects[object]->full_name));
#endif
}

bool UObjectHook::try_track_reachable_ui_object(sdk::UObjectBase* parent, sdk::UObjectBase* child, std::string_view context) {
    if (child == nullptr) {
        return false;
    }

    if (exists(child)) {
        return true;
    }

    if (parent != nullptr && !exists(parent)) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();

    {
        std::unique_lock _{m_mutex};

        if (m_last_ui_nested_resolve_refusal_prune.time_since_epoch().count() == 0 ||
            now - m_last_ui_nested_resolve_refusal_prune >= std::chrono::seconds(10))
        {
            for (auto it = m_ui_nested_resolve_refusals.begin(); it != m_ui_nested_resolve_refusals.end();) {
                if (now - it->second >= std::chrono::seconds(30)) {
                    it = m_ui_nested_resolve_refusals.erase(it);
                } else {
                    ++it;
                }
            }

            m_last_ui_nested_resolve_refusal_prune = now;
        }

        if (const auto refused = m_ui_nested_resolve_refusals.find(child); refused != m_ui_nested_resolve_refusals.end() &&
            now - refused->second < std::chrono::seconds(2))
        {
            ++m_ui_nested_resolve_stats.cached_refusals;
            return false;
        }

        ++m_ui_nested_resolve_stats.attempts;
    }

    const auto child_index_serial = get_uobject_index_serial(child);
    const auto safe_child = child_index_serial.has_value() && is_safe_uobject_candidate(*this, child, true);

    if (!safe_child) {
        {
            std::unique_lock _{m_mutex};
            m_ui_nested_resolve_refusals[child] = now;
            ++m_ui_nested_resolve_stats.refused;
        }

        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[UObjectHook] Refusing to adopt reachable UI UObject from {}: parent={:x} child={:x}",
            context,
            (uintptr_t)parent,
            (uintptr_t)child);
        return false;
    }

    // UI browsing can reveal valid objects that AddObject missed. Track metadata only;
    // do not run creation jobs from a passive explorer expansion.
    add_new_object(child, false);

    const auto tracked = exists(child);

    if (tracked) {
        {
            std::unique_lock _{m_mutex};
            m_ui_nested_resolve_refusals.erase(child);
            ++m_ui_nested_resolve_stats.adopted;
        }

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[UObjectHook] Adopted reachable UI UObject from {}: parent={:x} child={:x} index={} serial={}",
            context,
            (uintptr_t)parent,
            (uintptr_t)child,
            child_index_serial->first,
            child_index_serial->second);
    } else {
        std::unique_lock _{m_mutex};
        m_ui_nested_resolve_refusals[child] = now;
        ++m_ui_nested_resolve_stats.refused;
    }

    return tracked;
}

void UObjectHook::ui_handle_reachable_object(sdk::UObject* parent, sdk::UObject* child, std::string_view context) {
    if (child != nullptr && !exists(child)) {
        try_track_reachable_ui_object(parent, child, context);
    }

    ui_handle_object(child);
}

bool UObjectHook::try_track_object(
    sdk::UObjectBase* object,
    std::string_view context,
    bool require_array_member,
    bool run_creation_jobs) {
    if (object == nullptr) {
        return false;
    }

    const auto already_tracked = exists(object);

    if (!already_tracked) {
        if (!is_safe_uobject_candidate(*this, object, require_array_member)) {
            SPDLOG_WARNING_EVERY_N_SEC(
                2,
                "[UObjectHook] Refusing to adopt untracked UObject from {}: {:x}",
                context,
                (uintptr_t)object);
            return false;
        }

        add_new_object(object, run_creation_jobs);
    }

    const auto tracked = exists(object);

    if (tracked && !already_tracked) {
        SPDLOG_INFO("[UObjectHook] Adopted untracked UObject from {}: {:x}", context, (uintptr_t)object);
    } else if (!tracked) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[UObjectHook] Failed to adopt untracked UObject from {} after validation: {:x}",
            context,
            (uintptr_t)object);
    }

    if (tracked) {
        const auto object_class = object->get_class();

        if (object_class != nullptr &&
            (sdk::UObjectBase*)object_class != object &&
            !exists((sdk::UObjectBase*)object_class) &&
            is_probably_uobject_layout((sdk::UObjectBase*)object_class)) {
            // On-demand tracking skips the old bulk FUObjectArray backfill, so also
            // adopt the class metadata needed by the explorer to display the object.
            try_track_object((sdk::UObjectBase*)object_class, "tracked object class", false, false);
        }
    }

    return tracked;
}

void UObjectHook::mark_persistent_tracking_miss() {
    m_last_persistent_tracking_miss = std::chrono::steady_clock::now();
    ++m_uobject_array_scan_stats.persistent_tracking_misses;
}

void UObjectHook::prune_destroyed_object_tombstones(std::chrono::steady_clock::time_point now) {
    std::unique_lock _{m_mutex};

    if (m_destroyed_object_tombstones.empty()) {
        return;
    }

    if (m_last_tombstone_prune.time_since_epoch().count() != 0 && now - m_last_tombstone_prune < std::chrono::seconds(5)) {
        return;
    }

    m_last_tombstone_prune = now;

    for (auto it = m_destroyed_object_tombstones.begin(); it != m_destroyed_object_tombstones.end();) {
        if (it->second.time.time_since_epoch().count() == 0 || now - it->second.time > std::chrono::seconds(30)) {
            it = m_destroyed_object_tombstones.erase(it);
        } else {
            ++it;
        }
    }
}

bool UObjectHook::is_stalker2_bulk_scene_path(const StatePath& path) const {
    if (!is_stalker2_uobjecthook_guard_enabled()) {
        return false;
    }

    const auto& parts = path.path();

    for (auto it = parts.begin(); it != parts.end(); ++it) {
        if (*it != "Components") {
            continue;
        }

        const auto next = it + 1;

        if (next == parts.end()) {
            return false;
        }

        if (next->starts_with("SceneComponent ") ||
            next->starts_with("SceneComponent_") ||
            *next == "SceneComponent") {
            return true;
        }
    }

    return false;
}

void UObjectHook::request_stalker2_uobject_full_scan() {
    if (!is_stalker2_uobjecthook_guard_enabled()) {
        return;
    }

    m_stalker2_class_browser_enabled = true;
    m_stalker2_uobject_full_scan_requested.store(true, std::memory_order_relaxed);
    m_uobject_array_scan_cursor = 0;
    m_uobject_array_full_sweep_active = true;
    ++m_uobject_array_scan_stats.full_sweeps;
    SPDLOG_WARN("[Stalker2][UObjectHook] Explicit full FUObjectArray scan requested from UI; this can hitch while it backfills objects");
}

size_t UObjectHook::get_stalker2_bulk_scene_attachment_count() const {
    if (!is_stalker2_uobjecthook_guard_enabled()) {
        return 0;
    }

    size_t count = 0;
    std::shared_lock _{m_mutex};

    for (const auto& [component, state] : m_motion_controller_attached_components) {
        if (component != nullptr && state != nullptr && is_stalker2_bulk_scene_component(component)) {
            ++count;
        }
    }

    return count;
}

size_t UObjectHook::detach_non_persistent_motion_controller_states() {
    std::unique_lock _{m_mutex};

    size_t removed = 0;

    for (auto it = m_motion_controller_attached_components.begin(); it != m_motion_controller_attached_components.end();) {
        if (it->second == nullptr || !it->second->permanent) {
            it = m_motion_controller_attached_components.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    return removed;
}

size_t UObjectHook::detach_stalker2_bulk_scene_component_states() {
    std::vector<sdk::USceneComponent*> removed_components{};

    {
        std::unique_lock _{m_mutex};

        for (auto it = m_motion_controller_attached_components.begin(); it != m_motion_controller_attached_components.end();) {
            if (is_stalker2_bulk_scene_component(it->first)) {
                removed_components.push_back(it->first);
                it = m_motion_controller_attached_components.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (removed_components.empty()) {
        return 0;
    }

    const auto was_removed = [&removed_components](sdk::USceneComponent* component) {
        return std::find(removed_components.begin(), removed_components.end(), component) != removed_components.end();
    };

    for (auto it = m_persistent_states.begin(); it != m_persistent_states.end();) {
        const auto& state = *it;

        if (state == nullptr) {
            it = m_persistent_states.erase(it);
            continue;
        }

        const auto resolved = state->last_object != nullptr && was_removed(state->last_object) ?
            state->last_object :
            state->path.resolve(false).as<sdk::USceneComponent*>();

        if (resolved != nullptr && was_removed(resolved)) {
            state->erase_json_file();
            it = m_persistent_states.erase(it);
        } else {
            ++it;
        }
    }

    return removed_components.size();
}

uint32_t UObjectHook::get_uobject_array_scan_budget(sdk::UGameEngine* engine) {
    const bool force_scan = m_force_uobject_array_creation_scan.load(std::memory_order_relaxed);
    const bool add_object_guard_unreliable = m_add_object_guard_unreliable.load(std::memory_order_relaxed);

    if (!should_incrementally_refresh_uobject_array() && !force_scan && !add_object_guard_unreliable) {
        m_uobject_array_scan_stats.last_budget = 0;
        return 0;
    }

    auto object_array = sdk::FUObjectArray::get();

    if (object_array == nullptr) {
        m_uobject_array_scan_stats.last_budget = 0;
        return 0;
    }

    const auto object_count = object_array->get_object_count();

    if (object_count <= 0) {
        m_uobject_array_scan_stats.last_budget = 0;
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool stalker2_on_demand_tracking = should_use_stalker2_on_demand_uobject_tracking(object_count);

    if (m_uobject_array_scan_cursor < 0 || m_uobject_array_scan_cursor > object_count || object_count < m_uobject_array_last_object_count) {
        if (stalker2_on_demand_tracking) {
            m_uobject_array_scan_cursor = object_count;
            m_uobject_array_full_sweep_active = false;
        } else {
            m_uobject_array_scan_cursor = 0;
            m_uobject_array_full_sweep_active = true;
            ++m_uobject_array_scan_stats.full_sweeps;
        }
    }

    if (object_count > m_uobject_array_last_object_count && m_uobject_array_scan_cursor >= m_uobject_array_last_object_count) {
        // Prefer fresh indices first when the array grows; this catches new pawns/components without
        // continuously sweeping the whole FUObjectArray.
        m_uobject_array_scan_cursor = m_uobject_array_last_object_count;
    }

    m_uobject_array_last_object_count = object_count;

    bool pawn_tracking_unhealthy = false;

    if (engine != nullptr) {
        if (const auto world = engine->get_world(); world != nullptr) {
            if (const auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0); player_controller != nullptr) {
                if (stalker2_on_demand_tracking && !exists((sdk::UObjectBase*)player_controller)) {
                    try_track_object(
                        (sdk::UObjectBase*)player_controller,
                        "Stalker2 common root/PlayerController",
                        false,
                        false);
                }

                if (const auto pawn = player_controller->get_acknowledged_pawn(); pawn != nullptr && !exists(pawn)) {
                    if (stalker2_on_demand_tracking) {
                        if (try_track_object(
                                (sdk::UObjectBase*)pawn,
                                "Stalker2 common root/Acknowledged Pawn",
                                false,
                                false)) {
                            SPDLOG_INFO_EVERY_N_SEC(
                                5,
                                "[Stalker2][UObjectHook] Adopted acknowledged pawn directly without FUObjectArray backscan pawn={:x}",
                                (uintptr_t)pawn);
                        } else {
                            SPDLOG_WARNING_EVERY_N_SEC(
                                5,
                                "[Stalker2][UObjectHook] Acknowledged pawn is valid but direct tracking failed; not sweeping full FUObjectArray pawn={:x}",
                                (uintptr_t)pawn);
                        }
                    } else {
                        pawn_tracking_unhealthy = true;
                        m_last_untracked_pawn_seen = now;
                        SPDLOG_WARNING_EVERY_N_SEC(
                            5,
                            "[UObjectHook] Local pawn is valid but not tracked; raising FUObjectArray scan budget pawn={:x}",
                            (uintptr_t)pawn);
                    }
                }

                if (stalker2_on_demand_tracking) {
                    if (const auto camera_manager = player_controller->get_player_camera_manager();
                        camera_manager != nullptr && !exists((sdk::UObjectBase*)camera_manager)) {
                        try_track_object(
                            (sdk::UObjectBase*)camera_manager,
                            "Stalker2 common root/Camera Manager",
                            false,
                            false);
                    }
                }
            }

            if (stalker2_on_demand_tracking && !exists((sdk::UObjectBase*)world)) {
                try_track_object((sdk::UObjectBase*)world, "Stalker2 common root/World", false, false);
            }
        }
    }

    if (stalker2_on_demand_tracking) {
        // The Stalker2 object array is large enough that periodic full sweeps are visible
        // as VR hitches. Keep the common roots above tracked and do not backfill the old
        // FUObjectArray unless the user explicitly requests it from Objects by class.
        m_force_uobject_array_creation_scan.store(false, std::memory_order_relaxed);

        uint32_t budget = 0;

        if (m_stalker2_uobject_full_scan_requested.load(std::memory_order_relaxed)) {
            if (m_uobject_array_scan_cursor < object_count) {
                budget = STALKER2_FULL_SCAN_BUDGET;
                m_uobject_array_full_sweep_active = true;
            } else {
                m_stalker2_uobject_full_scan_requested.store(false, std::memory_order_relaxed);
                m_uobject_array_full_sweep_active = false;
            }
        } else {
            m_uobject_array_scan_cursor = object_count;
            m_uobject_array_last_object_count = object_count;
            m_uobject_array_full_sweep_active = false;
        }

        m_uobject_array_scan_stats.last_budget = budget;
        return budget;
    }

    const bool startup_window = now < m_uobject_array_startup_scan_until;
    const bool recent_tracking_miss =
        m_last_persistent_tracking_miss.time_since_epoch().count() != 0 &&
        now - m_last_persistent_tracking_miss < std::chrono::seconds(10);
    const bool high_priority =
        startup_window ||
        recent_tracking_miss ||
        pawn_tracking_unhealthy;

    uint32_t budget = 0;

    if (high_priority) {
        if (m_uobject_array_scan_cursor >= object_count) {
            m_uobject_array_scan_cursor = 0;
            m_uobject_array_full_sweep_active = true;
            ++m_uobject_array_scan_stats.full_sweeps;
        }

        // When AddObject is unreliable, scan fast enough to catch newly possessed pawns
        // without doing a permanent full-speed sweep every frame after gameplay starts.
        budget = add_object_guard_unreliable ? 4096 : 8192;
    } else if (m_uobject_array_scan_cursor < object_count) {
        budget = 1024;
    } else if (!m_uobject_array_full_sweep_active && now - m_last_uobject_array_full_sweep >= std::chrono::seconds(5)) {
        m_uobject_array_full_sweep_active = true;
        m_last_uobject_array_full_sweep = now;
        m_uobject_array_scan_cursor = 0;
        ++m_uobject_array_scan_stats.full_sweeps;
        budget = 512;
    } else if (m_uobject_array_full_sweep_active) {
        budget = 512;
    }

    m_uobject_array_scan_stats.last_budget = budget;
    return budget;
}

void UObjectHook::refresh_new_objects_from_uobject_array(uint32_t max_objects) {
    const bool force_scan = m_force_uobject_array_creation_scan.load(std::memory_order_relaxed);
    const bool add_object_guard_unreliable = m_add_object_guard_unreliable.load(std::memory_order_relaxed);

    if ((!should_incrementally_refresh_uobject_array() && !force_scan && !add_object_guard_unreliable) || max_objects == 0) {
        return;
    }

    auto object_array = sdk::FUObjectArray::get();

    if (object_array == nullptr) {
        return;
    }

    const auto object_count = object_array->get_object_count();

    if (object_count <= 0) {
        return;
    }

    if (m_uobject_array_scan_cursor < 0 || m_uobject_array_scan_cursor > object_count) {
        m_uobject_array_scan_cursor = 0;
    }

    const auto now = std::chrono::steady_clock::now();
    prune_destroyed_object_tombstones(now);
    const auto start = m_uobject_array_scan_cursor;
    const auto end = (int32_t)std::min<uint32_t>((uint32_t)object_count, (uint32_t)start + max_objects);
    uint32_t added = 0;
    uint32_t rejected = 0;
    uint32_t tombstone_skips = 0;

    ++m_uobject_array_scan_stats.ticks;
    m_uobject_array_scan_stats.scanned += (uint64_t)std::max(0, end - start);

    for (auto i = start; i < end; ++i) {
        auto item = object_array->get_object(i);

        if (item == nullptr) {
            continue;
        }

        auto object = item->get_object();

        if (object == nullptr || exists(object)) {
            continue;
        }

        bool skip_tombstoned_object = false;

        {
            std::unique_lock _{m_mutex};

            if (const auto tombstone_it = m_destroyed_object_tombstones.find(object); tombstone_it != m_destroyed_object_tombstones.end()) {
                const auto serial = item->get_serial_number();

                if (tombstone_it->second.index == i &&
                    tombstone_it->second.serial == serial &&
                    now - tombstone_it->second.time <= std::chrono::seconds(30))
                {
                    skip_tombstoned_object = true;
                } else {
                    m_destroyed_object_tombstones.erase(tombstone_it);
                }
            }
        }

        if (skip_tombstoned_object) {
            ++tombstone_skips;
            continue;
        }

        // Objects found directly through FUObjectArray must still pass layout and index validation.
        if (!is_safe_uobject_candidate(*this, object, true)) {
            ++rejected;
            continue;
        }

        add_new_object(object);

        if (exists(object)) {
            ++added;
        }
    }

    m_uobject_array_scan_cursor = end;
    m_uobject_array_scan_stats.added += added;
    m_uobject_array_scan_stats.rejected += rejected;
    m_uobject_array_scan_stats.tombstone_skips += tombstone_skips;

    if (m_uobject_array_scan_cursor >= object_count && m_uobject_array_full_sweep_active) {
        m_uobject_array_full_sweep_active = false;

        if (is_stalker2_uobjecthook_guard_enabled() &&
            m_stalker2_uobject_full_scan_requested.exchange(false, std::memory_order_relaxed)) {
            SPDLOG_INFO("[Stalker2][UObjectHook] Explicit full FUObjectArray scan completed");
        }
    }

    if (added > 0) {
        static auto last_log = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();

        if (now - last_log >= std::chrono::seconds(2)) {
            SPDLOG_INFO(
                "[UObjectHook] Incrementally tracked {} UObjectArray objects (cursor {}/{})",
                added,
                m_uobject_array_scan_cursor,
                object_count);
            last_log = now;
        }
    }
}

void UObjectHook::on_config_load(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    if (!set_defaults && m_enabled_at_startup->value()) {
        m_wants_activate = true;
    }
}

void UObjectHook::on_config_save(utility::Config& cfg) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

void UObjectHook::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    m_last_delta_time = delta;

    if (m_wants_activate) {
        hook();
    }
    
    if (m_fully_hooked) {
        {
            std::shared_lock _{m_mutex};
            const auto ui_active = g_framework->is_drawing_ui();

            for (auto& state : m_motion_controller_attached_components) {
                if (m_overlap_detection_actor == nullptr && state.second->adjusting && ui_active) {
                    state.second->adjusting = false;
                }
            }
        }

        if (const auto scan_budget = get_uobject_array_scan_budget(engine); scan_budget > 0) {
            refresh_new_objects_from_uobject_array(scan_budget);
        }
        update_persistent_states();
    }
}

const auto quat_converter = glm::quat{Matrix4x4f {
    0, 0, -1, 0,
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 0, 1
}};

// TODO: split this into some functions because its getting a bit massive
void UObjectHook::on_pre_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation, 
                                        const float world_to_meters, Vector3f* view_location, bool is_double)
{
    if (!m_fully_hooked) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    if (m_uobject_hook_disabled) {
        return;
    }

    auto view_d = (Vector3d*)view_location;
    auto rot_d = (Rotator<double>*)view_rotation;

    if (is_double) {
        m_last_camera_location = glm::vec3{*view_d};
    } else {
        m_last_camera_location = *view_location;
    }

    if (m_camera_attach.object != nullptr) {
        if (m_camera_attach.object->is_a(sdk::AActor::static_class())) {
            const auto actor = (sdk::AActor*)m_camera_attach.object;
            const auto location = actor->get_actor_location();
            const auto rotation = actor->get_actor_rotation();

            // Conv rotation to glm quat
            const auto rotation_glm_mat = glm::yawPitchRoll(
                glm::radians(-rotation.y),
                glm::radians(rotation.x),
                glm::radians(-rotation.z));
            const auto rotation_glm_quat = glm::quat{rotation_glm_mat};

            const auto adjusted_loc = location - (quat_converter * (rotation_glm_quat * utility::math::ue4_to_glm(m_camera_attach.offset)));

            if (is_double) {
                *view_d = glm::vec<3, double>{adjusted_loc};
            } else {
                *view_location = adjusted_loc;
            }
        } else if (m_camera_attach.object->is_a(sdk::USceneComponent::static_class())) {
            const auto comp = (sdk::USceneComponent*)m_camera_attach.object;
            const auto location = comp->get_world_location();
            const auto rotation = comp->get_world_rotation();

            // Conv rotation to glm quat
            const auto rotation_glm_mat = glm::yawPitchRoll(
                glm::radians(-rotation.y),
                glm::radians(rotation.x),
                glm::radians(-rotation.z));
            const auto rotation_glm_quat = glm::quat{rotation_glm_mat};

            const auto adjusted_loc = location - (quat_converter * (rotation_glm_quat * utility::math::ue4_to_glm(m_camera_attach.offset)));

            if (is_double) {
                *view_d = glm::vec<3, double>{adjusted_loc};
            } else {
                *view_location = adjusted_loc;
            }
        } // else todo?
    }

    if ((view_index + 1) % 2 == 0) {
        tick_attachments(view_rotation, world_to_meters, view_location, is_double);
    }
}

void UObjectHook::on_post_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation, 
                                                    const float world_to_meters, Vector3f* view_location, bool is_double)
{
    if (!m_fully_hooked) {
        return;
    }

    if (!VR::get()->is_hmd_active()) {
        return;
    }

    if (m_uobject_hook_disabled) {
        return;
    }

    std::shared_lock _{m_mutex};
    bool any_adjusting = false;
    for (auto& it : m_motion_controller_attached_components) {
        if (it.second->adjusting) {
            any_adjusting = true;
            break;
        }
    }

    VR::get()->set_aim_allowed(!any_adjusting);
}

void UObjectHook::tick_attachments(Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location, bool is_double) {
    if (m_uobject_hook_disabled) {
        return;
    }

    auto& vr = VR::get();
    auto view_d = (Vector3d*)view_location;
    auto rot_d = (Rotator<double>*)view_rotation;

    const auto view_mat_inverse = !is_double ? 
        glm::yawPitchRoll(
            glm::radians(-view_rotation->yaw),
            glm::radians(view_rotation->pitch),
            glm::radians(-view_rotation->roll)) : 
        glm::yawPitchRoll(
            glm::radians(-(float)rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians(-(float)rot_d->roll));

    const auto view_quat_inverse = glm::quat {
        view_mat_inverse
    };

    auto vqi_norm = glm::normalize(view_quat_inverse);
    
    // Decoupled Pitch
    if (vr->is_decoupled_pitch_enabled()) {
        vqi_norm = utility::math::flatten(vqi_norm);
    }

    const auto rotation_offset = vr->get_rotation_offset();
    const auto hmd_origin = glm::vec3{vr->get_transform(0)[3]};
    const auto pos = glm::vec3{rotation_offset * (hmd_origin - glm::vec3{vr->get_standing_origin()})};

    const auto adjusted_world_to_meters = world_to_meters * vr->get_world_scale();

    const auto view_quat_inverse_flat = utility::math::flatten(view_quat_inverse);
    const auto offset1 = quat_converter * (glm::normalize(view_quat_inverse_flat) * (pos * adjusted_world_to_meters));

    glm::vec3 final_position{};

    if (is_double) {
        final_position = glm::vec3{*view_d} - offset1;
    } else {
        final_position = *view_location - offset1;
    }

    auto with_mutex = [this](auto fn) {
        std::shared_lock _{m_mutex};
        auto result = fn();

        return result;
    };

    auto comps = with_mutex([this]() -> decltype(m_motion_controller_attached_components) {
        if (!is_stalker2_uobjecthook_guard_enabled()) {
            return m_motion_controller_attached_components;
        }

        if (m_motion_controller_attached_components.size() <= STALKER2_BULK_SCENE_ATTACHMENT_CAP) {
            return m_motion_controller_attached_components;
        }

        m_stalker2_lazy_stats.tick_bulk_skips.fetch_add(
            m_motion_controller_attached_components.size(),
            std::memory_order_relaxed);
        return {};
    });

    const auto is_using_controllers = vr->is_using_controllers();
    const auto has_any_head_components = std::any_of(comps.begin(), comps.end(), [](auto& it) { return it.second->hand == 2; });

    if (!is_using_controllers && !has_any_head_components) {
        return;
    }

    glm::vec3 right_hand_position = vr->get_controller_position_with_offset(VRRuntime::Hand::RIGHT, true);
    glm::quat right_hand_rotation = vr->get_controller_rotation_with_offset(VRRuntime::Hand::RIGHT);

    const float lerp_speed = m_attach_lerp_speed->value() * m_last_delta_time;

    if (m_attach_lerp_enabled->value()) {
        auto spherical_distance_right = glm::dot(right_hand_rotation, m_last_right_aim_rotation);

        if (spherical_distance_right < 0.0f) {
            spherical_distance_right = -spherical_distance_right;
        }

        const auto lenr = glm::max(1.0f, glm::length(right_hand_position - m_last_right_grip_location));
        m_last_right_grip_location = glm::lerp(m_last_right_grip_location, right_hand_position, glm::min(1.0f, lerp_speed * lenr));
        m_last_right_aim_rotation = glm::slerp(m_last_right_aim_rotation, right_hand_rotation, lerp_speed * spherical_distance_right);
        right_hand_position = m_last_right_grip_location;
        right_hand_rotation = m_last_right_aim_rotation;
    }

    const auto original_right_hand_rotation = right_hand_rotation;
    const auto original_right_hand_position = right_hand_position - hmd_origin;

    glm::vec3 left_hand_position = vr->get_controller_position_with_offset(VRRuntime::Hand::LEFT, true);
    glm::quat left_hand_rotation = vr->get_controller_rotation_with_offset(VRRuntime::Hand::LEFT);

    if (m_attach_lerp_enabled->value()) {
        auto spherical_distance_left = glm::dot(left_hand_rotation, m_last_left_aim_rotation);

        if (spherical_distance_left < 0.0f) {
            spherical_distance_left = -spherical_distance_left;
        }

        const auto lenl = glm::max(1.0f, glm::length(left_hand_position - m_last_left_grip_location));
        m_last_left_grip_location = glm::lerp(m_last_left_grip_location, left_hand_position, glm::min(1.0f, lerp_speed * lenl));
        m_last_left_aim_rotation = glm::slerp(m_last_left_aim_rotation, left_hand_rotation, lerp_speed * spherical_distance_left);
        left_hand_position = m_last_left_grip_location;
        left_hand_rotation = m_last_left_aim_rotation;
    }

    right_hand_position = glm::vec3{rotation_offset * (right_hand_position - hmd_origin)};
    left_hand_position = glm::vec3{rotation_offset * (left_hand_position - hmd_origin)};

    right_hand_position = quat_converter * (glm::normalize(view_quat_inverse_flat) * (right_hand_position * adjusted_world_to_meters));
    left_hand_position = quat_converter * (glm::normalize(view_quat_inverse_flat) * (left_hand_position * adjusted_world_to_meters));

    right_hand_position = final_position - right_hand_position;
    left_hand_position = final_position - left_hand_position;

    right_hand_rotation = rotation_offset * right_hand_rotation;
    right_hand_rotation = (glm::normalize(view_quat_inverse_flat) * right_hand_rotation);

    left_hand_rotation = rotation_offset * left_hand_rotation;
    left_hand_rotation = (glm::normalize(view_quat_inverse_flat) * left_hand_rotation);

    //right_hand_rotation = glm::normalize(right_hand_rotation * right_hand_offset_q);
    auto right_hand_euler = glm::degrees(utility::math::euler_angles_from_steamvr(right_hand_rotation));

    //left_hand_rotation = glm::normalize(left_hand_rotation * left_hand_offset_q);
    auto left_hand_euler = glm::degrees(utility::math::euler_angles_from_steamvr(left_hand_rotation));

    const auto head_rotation =  glm::normalize(vqi_norm * (rotation_offset * glm::quat{vr->get_rotation(0)}));
    const auto head_euler = glm::degrees(utility::math::euler_angles_from_steamvr(head_rotation));

    update_motion_controller_components(
        final_position, head_euler,
        left_hand_position, left_hand_euler, 
        right_hand_position, right_hand_euler);

    sdk::TArray<sdk::UPrimitiveComponent*> overlapped_components{};
    sdk::TArray<sdk::UPrimitiveComponent*> overlapped_components_left{};

    // Update overlapped components and overlap actor transform
    if (m_overlap_detection_actor != nullptr && this->exists(m_overlap_detection_actor)) {
        m_overlap_detection_actor->set_actor_location(right_hand_position, false, false);
        m_overlap_detection_actor->set_actor_rotation(right_hand_euler, false);

        if (!g_framework->is_drawing_ui()) {
            overlapped_components = std::move(m_overlap_detection_actor->get_overlapping_components());
        }
    }

    // Update overlapped components and overlap actor transform (left)
    if (m_overlap_detection_actor_left != nullptr && this->exists(m_overlap_detection_actor_left)) {
        m_overlap_detection_actor_left->set_actor_location(left_hand_position, false, false);
        m_overlap_detection_actor_left->set_actor_rotation(left_hand_euler, false);

        if (!g_framework->is_drawing_ui()) {
            overlapped_components_left = std::move(m_overlap_detection_actor_left->get_overlapping_components());
        }
    }

    // Check intuitive attachment for overlapped components
    if (!g_framework->is_drawing_ui() && (!overlapped_components.empty() || !overlapped_components_left.empty())) {
        static bool prev_right_a_pressed = false;
        static bool prev_left_a_pressed = false;
        const auto is_a_down_raw_right = vr->is_action_active_any_joystick(vr->get_action_handle(VR::s_action_a_button_right));
        const auto was_a_pressed_right = !prev_right_a_pressed && is_a_down_raw_right;

        const auto is_a_down_raw_left = vr->is_action_active_any_joystick(vr->get_action_handle(VR::s_action_a_button_left));
        const auto was_a_pressed_left = !prev_left_a_pressed && is_a_down_raw_left;

        prev_right_a_pressed = is_a_down_raw_right;
        prev_left_a_pressed = is_a_down_raw_left;

        // Update existing attached components before moving onto overlapped ones.
        for (auto& it : comps) {
            auto& state = *it.second;

            if (state.hand == (uint8_t)MotionControllerStateBase::Hand::LEFT) {
                if (is_a_down_raw_left) {
                    state.adjusting = true;
                } else if (!is_a_down_raw_left) {
                    state.adjusting = false;
                }
            } else if (state.hand == (uint8_t)MotionControllerStateBase::Hand::RIGHT) {
                if (is_a_down_raw_right) {
                    state.adjusting = true;
                } else if (!is_a_down_raw_right) {
                    state.adjusting = false;
                }
            } else {
                state.adjusting = false;
            }
        }
        
        auto update_overlaps = [&](int32_t hand, const sdk::TArray<sdk::UPrimitiveComponent*>& components) {
            const auto was_pressed = hand == 0 ? was_a_pressed_left : was_a_pressed_right;
            const auto is_pressed = hand == 0 ? is_a_down_raw_left : is_a_down_raw_right;

            for (auto overlap : components) {
                static const auto capsule_component_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.CapsuleComponent");
                if (overlap->get_class()->is_a(capsule_component_t)) {
                    continue;
                }

                {
                    std::shared_lock _{m_mutex};
                    if (m_spawned_spheres_to_components.contains(overlap)) {
                        overlap = (sdk::UPrimitiveComponent*)m_spawned_spheres_to_components[overlap];
                    }
                }

                const auto owner = overlap->get_owner();
                bool owner_is_adjustment_vis = false;

                if (owner == m_overlap_detection_actor || owner == m_overlap_detection_actor_left) {
                    continue;
                }

                // Make sure we don't try to attach to the adjustment visualizer
                {
                    std::shared_lock _{m_mutex};
                    auto it = std::find_if(m_motion_controller_attached_components.begin(), m_motion_controller_attached_components.end(),
                        [&](auto& it) {
                            if (it.second->adjustment_visualizer != nullptr && this->exists_unsafe(it.second->adjustment_visualizer)) {
                                if (it.second->adjustment_visualizer == owner) {
                                    owner_is_adjustment_vis = true;
                                    return true;
                                }
                            }

                            return false;
                        });
                    
                    if (owner_is_adjustment_vis) {
                        continue;
                    }
                }

                if (was_pressed) {
                    auto state = get_or_add_motion_controller_state(overlap);
                    state->adjusting = true;
                    state->hand = hand;
                } /*else if (!is_pressed) {
                    auto state = get_motion_controller_state(overlap);

                    if (state && (*state)->hand == hand) {
                        (*state)->adjusting = false;
                    }
                }*/
            }
        };

        update_overlaps(0, overlapped_components_left);
        update_overlaps(1, overlapped_components);
    }

    for (auto& it : comps) {
        if (!is_using_controllers && it.second->hand != 2) {
            continue;
        }

        auto comp = it.first;
        if (!this->exists(comp) || it.second == nullptr) {
            continue;
        }
        
        auto& state = *it.second;
        const auto orig_position = comp->get_world_location();
        const auto orig_rotation = comp->get_world_rotation();

        // Convert orig_rotation to quat
        const auto orig_rotation_mat = glm::yawPitchRoll(
            glm::radians(-orig_rotation.y),
            glm::radians(orig_rotation.x),
            glm::radians(-orig_rotation.z));
        const auto orig_rotation_quat = glm::quat{orig_rotation_mat};

        using Hand = MotionControllerStateBase::Hand;
        const auto& hand_rotation = state.hand != Hand::HMD ? (state.hand == Hand::RIGHT ? right_hand_rotation : left_hand_rotation) : head_rotation;
        const auto& hand_position = state.hand != Hand::HMD ? (state.hand == Hand::RIGHT ? right_hand_position : left_hand_position) : final_position;
        const auto& hand_euler = state.hand != Hand::HMD ? (state.hand == Hand::RIGHT ? right_hand_euler : left_hand_euler) : head_euler;

        const auto adjusted_rotation = hand_rotation * glm::inverse(state.rotation_offset);
        const auto adjusted_euler = glm::degrees(utility::math::euler_angles_from_steamvr(adjusted_rotation));
        const auto adjusted_location = hand_position + (quat_converter * (adjusted_rotation * state.location_offset));

        if (state.adjusting) {
            // Create a temporary actor that visualizes how we're adjusting the component
            if (state.adjustment_visualizer == nullptr) {
                auto ugs = sdk::UGameplayStatics::get();
                auto visualizer = ugs->spawn_actor(sdk::UGameEngine::get()->get_world(), sdk::AActor::static_class(), orig_position);

                if (visualizer != nullptr) {
                    auto add_comp = [&](sdk::UClass* c, std::function<void(sdk::UActorComponent*)> fn) -> sdk::UActorComponent* {
                        if (c == nullptr) {
                            SPDLOG_ERROR("[UObjectHook] Cannot add component of null class");
                            return nullptr;
                        }

                        auto new_comp = visualizer->add_component_by_class(c);

                        if (new_comp != nullptr) {
                            fn(new_comp);

                            if (new_comp->is_a(sdk::USceneComponent::static_class())) {
                                auto scene_comp = (sdk::USceneComponent*)new_comp;
                                scene_comp->set_hidden_in_game(false);
                            }

                            visualizer->finish_add_component(new_comp);
                        } else {
                            SPDLOG_ERROR("[UObjectHook] Failed to add component {} to adjustment visualizer", utility::narrow(c->get_full_name()));
                        }
                        
                        return new_comp;
                    };

                    add_comp(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SphereComponent"), [](sdk::UActorComponent* new_comp) {
                        struct SphereRadiusParams {
                            float radius{};
                            bool update_overlaps{false};
                        };

                        auto params = SphereRadiusParams{};
                        params.radius = 10.f;

                        const auto fn = new_comp->get_class()->find_function(L"SetSphereRadius");

                        if (fn != nullptr) {
                            new_comp->process_event(fn, &params);
                        }
                    });

                    /*add_comp(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.ArrowComponent"), [](sdk::UActorComponent* new_comp) {
                        struct {
                            float color[4]{1.0f, 0.0f, 0.0f, 1.0f};
                        } params{};

                        const auto fn = new_comp->get_class()->find_function(L"SetArrowColor");

                        if (fn != nullptr) {
                            new_comp->process_event(fn, &params);
                        }
                    });*/

                    // Ghetto way of making a "mesh" out of the box components
                    for (auto j = 0; j < 3; ++j) {
                        for (auto i = 1; i < 25; ++i) {
                            add_comp(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.BoxComponent"), [i, j](sdk::UActorComponent* new_comp) {
                                auto color = (uint8_t*)new_comp->get_property_data(L"ShapeColor");

                                if (color != nullptr) {
                                    color[0] = 255;
                                    color[1] = 255;
                                    color[2] = 0;
                                    color[3] = 0;
                                }

                                auto extent = (void*)new_comp->get_property_data(L"BoxExtent");

                                if (extent != nullptr) {
                                    static const bool is_ue5 = sdk::ScriptVector::static_struct()->get_struct_size() == sizeof(glm::vec<3, double>);

                                    const auto ratio = (float)i / 100.0f;

                                    const auto x = j == 0 ? ratio * 25.0f : 25.0f;
                                    const auto y = j == 1 ? ratio * 5.0f : 5.0f;
                                    const auto z = j == 2 ? ratio * 5.0f : 5.0f;

                                    glm::vec3 wanted_ext = glm::vec3{x, y, z};

                                    if (is_ue5) {
                                        *((glm::vec<3, double>*)extent) = wanted_ext;
                                    } else {
                                        *((glm::vec3*)extent) = wanted_ext;
                                    }
                                }
                            });
                        }
                    }

                    //ugs->finish_spawning_actor(visualizer, orig_position);

                    std::unique_lock _{m_mutex};
                    state.adjustment_visualizer = visualizer;
                } else {
                    SPDLOG_ERROR("[UObjectHook] Failed to spawn actor for adjustment visualizer");
                }
            } else {
                state.adjustment_visualizer->set_actor_location(hand_position, false, false);
                state.adjustment_visualizer->set_actor_rotation(hand_euler, false);
            }

            std::unique_lock _{m_mutex};
            const auto mat_inverse = 
                glm::yawPitchRoll(
                    glm::radians(-orig_rotation.y),
                    glm::radians(orig_rotation.x),
                    glm::radians(-orig_rotation.z));

            const auto mq = glm::quat{mat_inverse};
            const auto mqi = glm::inverse(mq);

            state.rotation_offset = mqi * hand_rotation;
            state.location_offset = mqi * utility::math::ue4_to_glm(hand_position - orig_position);
        } else {
            if (state.adjustment_visualizer != nullptr) {
                state.adjustment_visualizer->destroy_actor();

                std::unique_lock _{m_mutex};
                state.adjustment_visualizer = nullptr;
            }

            comp->set_world_location(adjusted_location, false, false);
            comp->set_world_rotation(adjusted_euler, false, false);
        }

        if (!state.permanent) {
            GameThreadWorker::get().enqueue([this, comp, orig_position, orig_rotation]() {
                if (!this->exists(comp)) {
                    return;
                }

                comp->set_world_location(orig_position, false, false);
                comp->set_world_rotation(orig_rotation, false, false);
            });
        }
    }
}

void UObjectHook::spawn_overlapper(uint32_t hand) {
    GameThreadWorker::get().enqueue([this, hand]() {
        auto ugs = sdk::UGameplayStatics::get();
        auto world = sdk::UGameEngine::get()->get_world();

        if (ugs == nullptr || world == nullptr) {
            return;
        }

        auto overlapper = ugs->spawn_actor(world, sdk::AActor::static_class(), glm::vec3{0, 0, 0});

        if (hand == 1) {
            if (m_overlap_detection_actor != nullptr && this->exists(m_overlap_detection_actor)) {
                m_overlap_detection_actor->destroy_actor();
            }

            m_overlap_detection_actor = overlapper;
        } else {
            if (m_overlap_detection_actor_left != nullptr && this->exists(m_overlap_detection_actor_left)) {
                m_overlap_detection_actor_left->destroy_actor();
            }

            m_overlap_detection_actor_left = overlapper;
        }

        if (overlapper != nullptr) {
            auto add_comp = [&](sdk::AActor* target, sdk::UClass* c, std::function<void(sdk::UActorComponent*)> fn) -> sdk::UActorComponent* {
                if (c == nullptr) {
                    SPDLOG_ERROR("[UObjectHook] Cannot add component of null class");
                    return nullptr;
                }

                auto new_comp = target->add_component_by_class(c);

                if (new_comp != nullptr) {
                    fn(new_comp);

                    if (new_comp->is_a(sdk::USceneComponent::static_class())) {
                        auto scene_comp = (sdk::USceneComponent*)new_comp;
                        scene_comp->set_hidden_in_game(false);
                    }

                    target->finish_add_component(new_comp);
                } else {
                    SPDLOG_ERROR("[UObjectHook] Failed to add component {} to target", utility::narrow(c->get_full_name()));
                }
                
                return new_comp;
            };

            const auto sphere_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SphereComponent");

            add_comp(overlapper, sphere_t, [](sdk::UActorComponent* new_comp) {
                struct SphereRadiusParams {
                    float radius{};
                    bool update_overlaps{true};
                } params{};

                params.radius = 10.0f;

                const auto fn = new_comp->get_class()->find_function(L"SetSphereRadius");

                if (fn != nullptr) {
                    new_comp->process_event(fn, &params);
                }
            });

            const auto skeletal_mesh_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");
            const auto meshes = get_objects_by_class(skeletal_mesh_t);
            
            for (auto obj : meshes) {
                if (!this->exists(obj)) {
                    continue;
                }

                // Dont add the same one twice
                if (m_components_with_spheres.contains((sdk::USceneComponent*)obj)) {
                    continue;
                }

                const auto default_obj = obj->get_class()->get_class_default_object();

                if (obj == default_obj) {
                    continue;
                }

                SPDLOG_INFO("[UObjectHook] Spawning sphere for skeletal mesh {}", utility::narrow(obj->get_full_name()));

                auto mesh = (sdk::USceneComponent*)obj;
                auto owner = mesh->get_owner();

                if (owner == nullptr) {
                    continue;
                }

                const auto owner_default = owner->get_class()->get_class_default_object();

                if (owner == owner_default) {
                    continue;
                }

                SPDLOG_INFO("[UObjectHook] Owner of skeletal mesh is {}", utility::narrow(owner->get_full_name()));

                auto new_sphere = (sdk::USceneComponent*)add_comp(owner, sphere_t, [](sdk::UActorComponent* new_comp) {
                    struct SphereRadiusParams {
                        float radius{};
                        bool update_overlaps{true};
                    } params{};

                    params.radius = 10.0f;

                    const auto fn = new_comp->get_class()->find_function(L"SetSphereRadius");

                    if (fn != nullptr) {
                        new_comp->process_event(fn, &params);
                    }
                });

                if (new_sphere != nullptr) {
                    new_sphere->attach_to(mesh, L"None", 0, true);
                    new_sphere->set_local_transform(glm::vec3{}, glm::vec4{0, 0, 0, 1}, glm::vec3{1, 1, 1});

                    std::unique_lock _{m_mutex};
                    m_spawned_spheres.insert(new_sphere);
                    m_spawned_spheres_to_components[new_sphere] = mesh;
                    m_components_with_spheres.insert(mesh);
                }
            }
        } else {
            SPDLOG_ERROR("[UObjectHook] Failed to spawn actor for overlapper");
        }
    });
}

void UObjectHook::destroy_overlapper() {
    GameThreadWorker::get().enqueue([this]() {
        // Destroy all spawned spheres
        auto spheres = get_spawned_spheres();

        for (auto sphere : spheres) {
            if (!this->exists(sphere)) {
                continue;
            }

            sphere->destroy_component();
        }

        {
            std::unique_lock _{m_mutex};
            m_spawned_spheres.clear();
            m_spawned_spheres_to_components.clear();
            m_components_with_spheres.clear();
        }

        if (m_overlap_detection_actor != nullptr && this->exists(m_overlap_detection_actor)) {
            m_overlap_detection_actor->destroy_actor();
            m_overlap_detection_actor = nullptr;
        }

        if (m_overlap_detection_actor_left != nullptr && this->exists(m_overlap_detection_actor_left)) {
            m_overlap_detection_actor_left->destroy_actor();
            m_overlap_detection_actor_left = nullptr;
        }
    });
}

std::filesystem::path UObjectHook::get_persistent_dir() {
    const auto base_dir = Framework::get_persistent_dir();
    const auto uobjecthook_dir = base_dir / "uobjecthook";

    try {
        if (!std::filesystem::exists(uobjecthook_dir)) {
            std::filesystem::create_directories(uobjecthook_dir);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to create persistent directory: {}", e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to create persistent directory");
    }

    return uobjecthook_dir;
}

nlohmann::json UObjectHook::serialize_mc_state(const std::vector<std::string>& path, const std::shared_ptr<MotionControllerState>& state) {
    nlohmann::json result{};

    result["path"] = path;
    result["state"] = state->to_json();
    result["type"] = "motion_controller";

    return result;
}

nlohmann::json UObjectHook::serialize_camera(const std::vector<std::string>& path) {
    nlohmann::json result{};

    result["path"] = path;
    result["offset"] = utility::math::to_json(m_camera_attach.offset);
    result["type"] = "camera";

    // todo: adjustments/offsets, etc...? all it needs is the camera object which is fine

    return result;
}

void UObjectHook::save_camera_state(const std::vector<std::string>& path) {
    auto json = serialize_camera(path);

    const auto wanted_dir = UObjectHook::get_persistent_dir() / "camera_state.json";

    // Create dir if necessary
    try {
        std::filesystem::create_directories(wanted_dir.parent_path());

        if (std::filesystem::exists(wanted_dir.parent_path())) {
            std::ofstream file{wanted_dir};
            file << json.dump(4);
            file.close();

            m_persistent_camera_state = deserialize_camera_state();
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to save camera state: {}", e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to save camera state");
    }
}

std::optional<UObjectHook::StatePath> UObjectHook::deserialize_path(const nlohmann::json& data) {
    if (!data.contains("path")) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (missing path)");
        return std::nullopt;
    }

    // make sure path is an array
    if (!data["path"].is_array()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (path is not an array)");
        return std::nullopt;
    }

    const auto path = data["path"].get<std::vector<std::string>>();

    if (path.empty()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (path is empty)");
        return std::nullopt;
    }

    return StatePath{path};
}

std::shared_ptr<UObjectHook::PersistentState> UObjectHook::deserialize_mc_state(nlohmann::json& data) {
    SPDLOG_INFO("[UObjectHook] inside deserialize_mc_state");

    if (!data.contains("path") || !data.contains("state")) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (missing path or state)");
        return nullptr;
    }

    // make sure path is an array
    if (!data["path"].is_array()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (path is not an array)");
        return nullptr;
    }

    // make sure state is an object
    if (!data["state"].is_object()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (state is not an object)");
        return nullptr;
    }

    if (data.contains("type") && data["type"].is_string()) {
        const auto type = data["type"].get<std::string>();

        if (type != "motion_controller") {
            SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (type is not motion_controller)");
            return nullptr;
        }
    }

    SPDLOG_INFO("[UObjectHook] Deserializing state path...");
    auto path = data["path"].get<std::vector<std::string>>();

    auto persistent_state = std::make_shared<PersistentState>();
    persistent_state->path = path;

    SPDLOG_INFO("[UObjectHook] Deserializing state...");
    persistent_state->state.from_json(data["state"]);

    return persistent_state;
}

std::shared_ptr<UObjectHook::PersistentState> UObjectHook::deserialize_mc_state(std::filesystem::path json_path) {
    if (!std::filesystem::exists(json_path)) {
        return nullptr;
    }

    try {
        auto f = std::ifstream{json_path};

        if (f.is_open()) {
            // Log the file data to make sure we're getting it correctly...
            const auto file_contents = std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};

            SPDLOG_INFO("[UObjectHook] JSON file contents:");
            SPDLOG_INFO("{}", file_contents);

            nlohmann::json data = nlohmann::json::parse(file_contents);

            return deserialize_mc_state(data);
        }

        SPDLOG_ERROR("[UObjectHook] Failed to open JSON file {}", json_path.string());
        return nullptr;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}: {}", json_path.string(), e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}", json_path.string());
    }

    return nullptr;
}

std::vector<std::shared_ptr<UObjectHook::PersistentState>> UObjectHook::deserialize_all_mc_states() try {
    const auto uobjecthook_dir = get_persistent_dir();

    if (!std::filesystem::exists(uobjecthook_dir)) {
        return {};
    }
    
    // Gather all .json files in this directory
    std::vector<std::filesystem::path> json_files{};
    for (const auto& p : std::filesystem::directory_iterator(uobjecthook_dir)) {
        if (p.path().extension() == ".json") {
            json_files.push_back(p.path());
        }
    }

    std::vector<std::shared_ptr<PersistentState>> result{};
    for (const auto& json_file : json_files) {
        auto state = deserialize_mc_state(json_file);

        if (state != nullptr) {
            state->path_to_json = json_file;
            result.push_back(state);
        }
    }

    return result;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize all motion controller states: {}", e.what());
    return {};
} catch (...) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize all motion controller states");
    return {};
}

std::shared_ptr<UObjectHook::PersistentCameraState> UObjectHook::deserialize_camera(const nlohmann::json& data) {
    const auto path = deserialize_path(data);

    if (!path.has_value()) {
        SPDLOG_ERROR("[UObjectHook] Failed to deserialize camera path");
        return nullptr;
    }

    if (!data.contains("type") || !data["type"].is_string()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (missing type)");
        return nullptr;
    }

    if (data["type"].get<std::string>() != "camera") {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (type is not camera)");
        return nullptr;
    }

    auto persistent_state = std::make_shared<PersistentCameraState>();
    persistent_state->path = path.value();

    if (data.contains("offset") && data["offset"].is_object()) {
        persistent_state->offset = utility::math::from_json_vec3(data["offset"]);
    }

    return persistent_state;
}

std::shared_ptr<UObjectHook::PersistentCameraState> UObjectHook::deserialize_camera_state() {
    // Look for camera_state.json
    const auto uobjecthook_dir = get_persistent_dir();
    const auto camera_state_path = uobjecthook_dir / "camera_state.json";

    if (!std::filesystem::exists(camera_state_path)) {
        SPDLOG_ERROR("[UObjectHook] Failed to find camera_state.json");
        return nullptr;
    }

    try {
        auto f = std::ifstream{camera_state_path};

        if (f.is_open()) {
            // Log the file data to make sure we're getting it correctly...
            const auto file_contents = std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};

            SPDLOG_INFO("[UObjectHook] JSON file contents:");
            SPDLOG_INFO("{}", file_contents);

            nlohmann::json data = nlohmann::json::parse(file_contents);

            auto result = deserialize_camera(data);

            if (result != nullptr) {
                result->path_to_json = camera_state_path;
            }

            return result;
        }

        SPDLOG_ERROR("[UObjectHook] Failed to open JSON file {}", camera_state_path.string());
        return nullptr;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}: {}", camera_state_path.string(), e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}", camera_state_path.string());
    }

    return nullptr;
}

void UObjectHook::update_persistent_states() {
    if (m_uobject_hook_disabled && m_fixed_visibilities) {
        return;
    }

    // For when we disable UObjectHook
    utility::ScopeGuard ___{[this]() {
        m_fixed_visibilities = true;
    }};

    bool had_tracking_miss = false;

    // Camera state
    if (m_persistent_camera_state != nullptr) {
        auto obj = m_persistent_camera_state->path.resolve(true);

        if (obj != nullptr) {
            m_camera_attach.object = obj;
            m_camera_attach.offset = m_persistent_camera_state->offset;
        } else {
            had_tracking_miss = true;
        }
    }

    // Motion controller states
    if (!m_persistent_states.empty()) {
        const bool stalker2_lazy = is_stalker2_uobjecthook_guard_enabled();
        size_t stalker2_bulk_scene_attachments = stalker2_lazy ?
            STALKER2_BULK_SCENE_ATTACHMENT_CAP :
            0;

        auto handle_persistent_state = [&](const std::shared_ptr<PersistentState>& state) {
            if (state == nullptr) {
                return;
            }

            if (stalker2_lazy && is_stalker2_bulk_scene_path(state->path)) {
                m_stalker2_lazy_stats.persistent_path_skips.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            auto obj = state->path.resolve(true);

            if (obj == nullptr) {
                had_tracking_miss = true;
                return;
            }

            static const auto scene_component_t = sdk::USceneComponent::static_class();

            // TODO? will need some reworking to support properties from arbitrary objects
            if (!obj.definition->is_a(scene_component_t)) {
                return;
            }

            const auto scene_component = obj.as<sdk::USceneComponent*>();
            const auto is_stalker2_bulk_scene_attachment = is_stalker2_bulk_scene_component(scene_component);
            const auto existing_motion_state = get_motion_controller_state(scene_component);

            if (is_stalker2_bulk_scene_attachment &&
                !existing_motion_state.has_value() &&
                stalker2_bulk_scene_attachments >= STALKER2_BULK_SCENE_ATTACHMENT_CAP) {
                m_stalker2_lazy_stats.persistent_bulk_skips.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // Destroy the existing mc state if it exists
            // This can cause issues if the previous object still exists
            // so we need to detach the old one
            if (state->last_object != nullptr && state->last_object != obj) {
                remove_motion_controller_state(state->last_object);
            }

            auto mc_state = get_or_add_motion_controller_state(scene_component);

            if (mc_state == nullptr) {
                return;
            }

            if (is_stalker2_bulk_scene_attachment && !existing_motion_state.has_value()) {
                ++stalker2_bulk_scene_attachments;
            }

            if (mc_state->adjusting) {
                state->state = *mc_state;
            } else {
                *mc_state = state->state;
            }

            state->last_object = scene_component;
        };

        if (stalker2_lazy) {
            if (m_stalker2_persistent_state_cursor >= m_persistent_states.size()) {
                m_stalker2_persistent_state_cursor = 0;
            }

            const auto end = std::min(
                m_persistent_states.size(),
                m_stalker2_persistent_state_cursor + STALKER2_PERSISTENT_STATE_RESOLVE_BUDGET);

            for (auto i = m_stalker2_persistent_state_cursor; i < end; ++i) {
                handle_persistent_state(m_persistent_states[i]);
            }

            if (end < m_persistent_states.size()) {
                m_stalker2_lazy_stats.persistent_budget_skips.fetch_add(
                    m_persistent_states.size() - end,
                    std::memory_order_relaxed);
            }

            m_stalker2_persistent_state_cursor = end >= m_persistent_states.size() ? 0 : end;
        } else {
            for (const auto& state : m_persistent_states) {
                handle_persistent_state(state);
            }
        }
    }

    // Persistent properties
    if (!m_persistent_properties.empty()) {
        const bool stalker2_lazy = is_stalker2_uobjecthook_guard_enabled();
        const auto scene_comp_t = sdk::USceneComponent::static_class();
        const auto primitive_comp_t = sdk::UPrimitiveComponent::static_class();
        auto handle_persistent_properties = [&](const std::shared_ptr<PersistentProperties>& prop_base) {
            if (prop_base == nullptr) {
                return;
            }

            if (stalker2_lazy && is_stalker2_bulk_scene_path(prop_base->path)) {
                m_stalker2_lazy_stats.persistent_path_skips.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            auto obj = prop_base->path.resolve(true);

            if (obj == nullptr) {
                had_tracking_miss = true;
                return;
            }

            if (prop_base->hide) {
                if (obj.definition->is_a(primitive_comp_t)) {
                    auto obj_primcomp = obj.as<sdk::UPrimitiveComponent*>();

                    if (m_uobject_hook_disabled) {
                        obj_primcomp->set_overall_visibility(true, true);
                    } else {
                        obj_primcomp->set_overall_visibility(false, prop_base->hide_legacy);
                    }
                } else if (obj.definition->is_a(scene_comp_t)) {
                    auto obj_scenecomp = obj.as<sdk::USceneComponent*>();

                    if (m_uobject_hook_disabled) {
                        obj_scenecomp->set_visibility(true, false);
                    } else {
                        obj_scenecomp->set_visibility(false, false);
                    }
                }
            }

            for (const auto& prop_state : prop_base->properties) {
                const auto prop_desc = obj.definition->find_property(prop_state->name);
            
                if (prop_desc == nullptr) {
                    continue;
                }

                const auto prop_t = prop_desc->get_class();

                if (prop_t == nullptr) {
                    continue;
                }

                const auto prop_t_name = prop_t->get_name().to_string();

                switch (utility::hash(utility::narrow(prop_t_name))) {
                case "FloatProperty"_fnv:
                    {
                        auto& value = *(float*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.f;
                    }
                    break;
                case "DoubleProperty"_fnv:
                    {
                        auto& value = *(double*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.d;
                    }
                    break;
                case "UInt32Property"_fnv:
                case "IntProperty"_fnv:
                    {
                        auto& value = *(int32_t*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.i;
                    }
                    break;
                case "BoolProperty"_fnv:
                    {
                        auto boolprop = (sdk::FBoolProperty*)prop_desc;
                        boolprop->set_value_in_object(obj, prop_state->data.b);
                    }
                    break;
                case "ByteProperty"_fnv:
                    {
                        auto& value = *(uint8_t*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.u8;
                    }
                    break;
                case "UInt16Property"_fnv:
                    {
                        auto& value = *(uint16_t*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.u16;
                    }
                    break;
                default:
                    // OH NO!!!!! anyways
                    break;
                };
            }
        };

        if (stalker2_lazy) {
            if (m_stalker2_persistent_property_cursor >= m_persistent_properties.size()) {
                m_stalker2_persistent_property_cursor = 0;
            }

            const auto end = std::min(
                m_persistent_properties.size(),
                m_stalker2_persistent_property_cursor + STALKER2_PERSISTENT_PROPERTY_RESOLVE_BUDGET);

            for (auto i = m_stalker2_persistent_property_cursor; i < end; ++i) {
                handle_persistent_properties(m_persistent_properties[i]);
            }

            if (end < m_persistent_properties.size()) {
                m_stalker2_lazy_stats.persistent_budget_skips.fetch_add(
                    m_persistent_properties.size() - end,
                    std::memory_order_relaxed);
            }

            m_stalker2_persistent_property_cursor = end >= m_persistent_properties.size() ? 0 : end;
        } else {
            for (const auto& prop_base : m_persistent_properties) {
                handle_persistent_properties(prop_base);
            }
        }
    }

    if (had_tracking_miss) {
        mark_persistent_tracking_miss();
    }
}

void UObjectHook::update_motion_controller_components(
    const glm::vec3& hmd_location, const glm::vec3& hmd_euler,
    const glm::vec3& left_hand_location, const glm::vec3& left_hand_euler,
    const glm::vec3& right_hand_location, const glm::vec3& right_hand_euler) 
{
    const auto mc_c = sdk::UMotionControllerComponent::static_class();

    if (mc_c == nullptr) {
        SPDLOG_ERROR_ONCE("[UObjectHook] Failed to find MotionControllerComponent class, cannot update motion controller components");
        return;
    }

    const auto motion_controllers = this->get_objects_by_class(mc_c);

    if (motion_controllers.empty()) {
        return;
    }

    for (auto obj : motion_controllers) {
        auto mc = (sdk::UMotionControllerComponent*)obj;
        if (!this->exists(mc)) {
            continue;
        }

        if (mc == mc_c->get_class_default_object()) {
            continue;
        }

        if (mc->get_outer() == nullptr || !this->exists(mc->get_outer())) {
            continue;
        }

        if (!sdk::UObjectReference{mc}.valid()) {
            continue;
        }

        if (mc->get_player_index() > 0) {
            continue;
        }

        if (mc->has_motion_source()) {
            const auto& motion_source = mc->get_motion_source();
            const auto motion_source_str = motion_source.to_string();

            if (motion_source_str == L"Left") {
                mc->set_world_location(left_hand_location, false, false);
                mc->set_world_rotation(left_hand_euler, false);
            } else if (motion_source_str == L"Right") {
                mc->set_world_location(right_hand_location, false, false);
                mc->set_world_rotation(right_hand_euler, false);
            } else if (motion_source_str == L"Head" || motion_source_str == L"HMD") {
                mc->set_world_location(hmd_location, false, false);
                mc->set_world_rotation(hmd_euler, false);
            }
        } else {
            if (mc->get_hand() == sdk::EControllerHand::Left) {
                mc->set_world_location(left_hand_location, false, false);
                mc->set_world_rotation(left_hand_euler, false);
            } else if (mc->get_hand() == sdk::EControllerHand::Right) {
                mc->set_world_location(right_hand_location, false, false);
                mc->set_world_rotation(right_hand_euler, false);
            }
        }
    }
}

sdk::UObject* UObjectHook::StatePath::resolve_base_object() const {
    if (!this->has_valid_base()) {
        return nullptr;
    }

    auto engine = sdk::UGameEngine::get();
    if (engine == nullptr) {
        return nullptr;
    }

    // TODO: Convert these into an enum or something when we initially parse the JSON file.
    switch (utility::hash(m_path.front())) {
    case "Acknowledged Pawn"_fnv:
    {
        auto world = engine->get_world();
        if (world == nullptr) {
            return nullptr;
        }

        auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

        if (player_controller == nullptr) {
            return nullptr;
        }
        
        return player_controller->get_acknowledged_pawn();
        break;
    }

    case "Player Controller"_fnv:
    {
        auto world = engine->get_world();
        if (world == nullptr) {
            return nullptr;
        }

        return sdk::UGameplayStatics::get()->get_player_controller(world, 0);
        break;
    }

    case "Camera Manager"_fnv:
    {      
        auto world = engine->get_world();
        if (world == nullptr) {
            return nullptr;
        }

        auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

        if (player_controller == nullptr) {
            return nullptr;
        }
        
        return player_controller->get_player_camera_manager();
        break;
    }

    case "World"_fnv:
    {
        return engine->get_world();
        break;
    }

    default:
        break;
    };

    return nullptr;
}

UObjectHook::ResolvedObject UObjectHook::StatePath::resolve(bool require_tracked_objects) const {
    const auto base = resolve_base_object();

    if (base == nullptr) {
        return nullptr;
    }

    const auto hook = UObjectHook::get();
    const auto is_stalker2_acknowledged_pawn_path =
        is_stalker2_uobjecthook_guard_enabled() &&
        !m_path.empty() &&
        m_path.front() == "Acknowledged Pawn";

    if (require_tracked_objects && !hook->try_track_object(base, "persistent path base", true)) {
        if (is_stalker2_acknowledged_pawn_path) {
            SPDLOG_WARNING_EVERY_N_SEC(
                2,
                "[Stalker2][UObjectHook] Deferring persistent Acknowledged Pawn path because the pawn is not tracked yet");
        }

        return nullptr;
    }

    void* previous_data = base;
    sdk::UStruct* previous_data_desc = base->get_class();

    if (previous_data_desc == nullptr) {
        return nullptr;
    }

    for (auto it = m_path.begin() + 1; it != m_path.end(); ++it) {
        if (previous_data == nullptr || previous_data_desc == nullptr) {
            return nullptr;
        }

        switch (utility::hash(*it)) {
        case "Outer"_fnv:
        {
            static const auto object_t = sdk::UObject::static_class();

            if (!previous_data_desc->is_a(object_t)) {
                return nullptr;
            }

            previous_data = ((sdk::UObject*)previous_data)->get_outer();
            if (previous_data != nullptr) {
                previous_data_desc = ((sdk::UObject*)previous_data)->get_class();
            } else {
                previous_data_desc = nullptr;
            }

            break;
        }
        case "Components"_fnv:
        {
            // Make sure the base is an AActor
            static const auto actor_t = sdk::AActor::static_class();

            if (!previous_data_desc->is_a(actor_t)) {
                return nullptr;
            }

            const auto components = ((sdk::AActor*)previous_data)->get_all_components();

            if (components.empty()) {
                return nullptr;
            }

            auto next_it = it + 1;

            if (next_it == m_path.end()) {
                return nullptr;
            }

            bool found = false;

            for (auto comp : components) {
                if (comp == nullptr) {
                    continue;
                }

                const auto& comp_fname = comp->get_fname();
                const auto comp_name = comp_fname.to_string_remove_numbers();
                const auto comp_ends_with_number = comp_fname.get_number() != 0;

                const auto comp_expanded_name = utility::narrow(comp->get_class()->get_fname().to_string() + L" " + comp_name);
                const auto is_match = comp_ends_with_number ? next_it->starts_with(comp_expanded_name) 
                                                            : *next_it == comp_expanded_name;

                if (is_match) {
                    if (require_tracked_objects && !hook->try_track_object(comp, "persistent path component", true)) {
                        return nullptr;
                    }

                    found = true;
                    previous_data = comp;
                    previous_data_desc = comp->get_class();
                    ++it;
                    break;
                }
            }

            if (!found) {
                return nullptr;
            }

            break;
        }
        case "Properties"_fnv:
        {
            auto next_it = it + 1;

            if (next_it == m_path.end()) {
                return nullptr;
            }

            const auto prop_name = *next_it;
            const auto prop_desc = previous_data_desc->find_property(utility::widen(prop_name));

            if (prop_desc == nullptr) {
                return nullptr;
            }

            const auto prop_t = prop_desc->get_class();

            if  (prop_t == nullptr) {
                return nullptr;
            }

            const auto prop_t_name = prop_t->get_name().to_string();
            switch (utility::hash(utility::narrow(prop_t_name))) {
            case "InterfaceProperty"_fnv:
            case "ObjectProperty"_fnv:
            {
                const auto obj_ptr = prop_desc->get_data<sdk::UObject*>(previous_data);
                const auto obj = obj_ptr != nullptr ? *obj_ptr : nullptr;

                if (obj == nullptr) {
                    return nullptr;
                }

                if (require_tracked_objects && !hook->try_track_object(obj, "persistent path object property", true)) {
                    return nullptr;
                }

                previous_data = obj;
                previous_data_desc = obj->get_class();
                ++it;
                break;
            }
            case "StructProperty"_fnv:
            {
                const auto struct_data = prop_desc->get_data<void*>(previous_data);

                previous_data = struct_data;
                previous_data_desc = ((sdk::FStructProperty*)prop_desc)->get_struct();

                if (previous_data_desc == nullptr || previous_data == nullptr) {
                    return nullptr;
                }

                // ++it because we are examining the properties
                ++it;
                break;
            }
            case "ArrayProperty"_fnv:
            {
                const auto inner = ((sdk::FArrayProperty*)prop_desc)->get_inner();

                if (inner == nullptr) {
                    return nullptr;
                }
                
                const auto inner_c = inner->get_class();

                if (inner_c == nullptr) {
                    return nullptr;
                }

                const auto inner_c_type = utility::narrow(inner_c->get_name().to_string());

                // only support ObjectProperty for now
                if (inner_c_type != "ObjectProperty" && inner_c_type != "InterfaceProperty") {
                    return nullptr;
                }

                const auto array_ptr = prop_desc->get_data<sdk::TArray<sdk::UObject*>>(previous_data);

                if (array_ptr == nullptr) {
                    return nullptr;
                }

                const auto& arr = *array_ptr;

                if (arr.empty() || arr.data == nullptr) {
                    return nullptr;
                }

                auto prop_it = next_it + 1;

                if (prop_it == m_path.end()) {
                    return nullptr;
                }

                bool found = false;

                // Now look for the object in the array
                for (auto obj : arr) {
                    if (obj == nullptr) {
                        continue;
                    }

                    const auto& obj_fname = obj->get_fname();
                    const auto obj_name = obj_fname.to_string_remove_numbers();
                    const auto obj_ends_with_number = obj_fname.get_number() != 0;

                    const auto obj_expanded_name = utility::narrow(obj->get_class()->get_fname().to_string() + L" " + obj_name);
                    const auto is_match = obj_ends_with_number ? prop_it->starts_with(obj_expanded_name) 
                                                               : *prop_it == obj_expanded_name;

                    if (is_match) {
                        if (require_tracked_objects && !hook->try_track_object(obj, "persistent path array object", true)) {
                            return nullptr;
                        }

                        found = true;
                        previous_data = obj;
                        previous_data_desc = obj->get_class();
                        ++it;
                        ++it;
                        break;
                    }
                }

                if (!found) {
                    return nullptr;
                }
                break;
            }

            default:
                SPDLOG_ERROR("[UObjectHook] Unsupported persistent property type {}", utility::narrow(prop_t_name));
                break;
            };

            break;
        }

        default:
            return nullptr;
            break;
        };
    }

    if (previous_data == nullptr || previous_data_desc == nullptr) {
        return nullptr;
    }

    return ResolvedObject{(sdk::UObject*)previous_data, previous_data_desc};
}

void UObjectHook::on_frame() {
    if (m_keybind_toggle_uobject_hook->is_key_down_once()) {
        set_disabled(!is_disabled());
    }
}

void UObjectHook::on_draw_ui() {
    activate();

    if (!m_fully_hooked) {
        ImGui::Text("Waiting for UObjectBase to be hooked...");
        return;
    }

    std::shared_lock _{m_mutex};
    std::scoped_lock __{m_function_mutex};

    if (m_uobject_hook_disabled) {
        ImGui::TextColored(ImVec4{1.0f, 0.0f, 0.0f, 1.0f}, "UObjectHook is disabled");
        if (ImGui::Button("Re-enable")) {
            m_uobject_hook_disabled = false;
        }
        return;
    }

    if (ImGui::Button("Reload Persistent States")) {
        reload_persistent_states();
    }

    ImGui::SameLine();

    if (ImGui::Button("Destroy Persistent States")) {
        reset_persistent_states();

        const auto uobjecthook_dir = get_persistent_dir();

        if (std::filesystem::exists(uobjecthook_dir)) {
            for (const auto& p : std::filesystem::directory_iterator(uobjecthook_dir)) {
                if (p.path().extension() == ".json") {
                    std::filesystem::remove(p.path());
                }
            }
        }
    }
}

void UObjectHook::on_draw_sidebar_entry(std::string_view in_entry) {
    on_draw_ui();
    ImGui::Separator();

    if (in_entry == "Main") {
        draw_main();
    } else if (in_entry == "Config") {
        draw_config();
    } else if (in_entry == "Developer") {
        draw_developer();
    }
}

void UObjectHook::draw_config() {
    m_enabled_at_startup->draw("Enabled at Startup");
    m_attach_lerp_enabled->draw("Enable Attach Lerp");
    m_attach_lerp_speed->draw("Attach Lerp Speed");

    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (ImGui::TreeNode("UObjectHook Keybinds")) {
        m_keybind_toggle_uobject_hook->draw("Disable UObjectHook Key");

        ImGui::TreePop();
    }
}

void UObjectHook::draw_developer() {
    if (ImGui::Button("Dump SDK")) {
        SDKDumper::dump();
    }

    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (ImGui::TreeNode("Debug Stats")) {
        // uint64_t
        ImGui::Text("Constructor calls: %llu", m_debug.constructor_calls);
        ImGui::Text("Destructor calls: %llu", m_debug.destructor_calls);

        ImGui::TreePop();
    }

    ImGui::Separator();

    if (!m_attempted_hook_process_event) {
        if (ImGui::Button("Create ProcessEvent hook")) {
            GameThreadWorker::get().enqueue([this]() {
                hook_process_event();
            });
        }
    } else if (m_hooked_process_event) {
        ImGui::Checkbox("ProcessEvent Listener", &m_process_event_listening);

        if (m_process_event_listening) {
            if (ImGui::Button("Clear Ignored Functions")) {
                m_ignored_recent_functions.clear();
            }

            ImGui::SameLine();

            std::scoped_lock __{m_function_mutex};

            if (ImGui::Button("Clear Called Functions")) {
                m_called_functions.clear();
                m_most_recent_functions.clear();
            }

            if (ImGui::Button("Ignore All Called Functions")) {
                m_ignored_recent_functions.clear();

                for (auto& [ufunc, data] : m_called_functions) {
                    m_ignored_recent_functions.insert(ufunc);
                }
            }

            ImGui::Text("Called functions: %llu", m_called_functions.size());

            std::vector<sdk::UFunction*> functions_to_cleanup{};

            if (ImGui::TreeNode("Recent Functions")) {
                for (auto ufunc : m_most_recent_functions) {
                    if (ufunc == nullptr) {
                        continue;
                    }
                    
                    if (m_ignored_recent_functions.contains(ufunc)) {
                        continue;
                    }

                    if (!this->exists(ufunc)) {
                        functions_to_cleanup.push_back(ufunc);
                        continue;
                    }

                    ImGui::PushID(ufunc);

                    utility::ScopeGuard ___{[]() {
                        ImGui::PopID();
                    }};

                    if (ImGui::Button("Ignore")) {
                        m_ignored_recent_functions.insert(ufunc);
                    }

                    ImGui::SameLine();

                    ImGui::Text("%s", utility::narrow(ufunc->get_full_name()).c_str());
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("All Called Functions")) {
                ImGui::SliderInt("Max Calls", &m_process_event_search.max_calls, 0, 10000);
                ImGui::InputText("Search", m_process_event_search.buffer.data(), m_process_event_search.buffer.size());

                std::string_view search{m_process_event_search.buffer.data()};
                std::vector<sdk::UFunction*> functions_sorted_by_call_count{};
                
                for (auto& [ufunc, data] : m_called_functions) {
                    if (ufunc == nullptr) {
                        continue;
                    }

                    if (!this->exists(ufunc)) {
                        functions_to_cleanup.push_back(ufunc);
                        continue;
                    }

                    if (m_process_event_search.max_calls > 0 && data.call_count > m_process_event_search.max_calls) {
                        continue;
                    }

                    // maybe a TODO here for optimization
                    if (!search.empty() && !utility::narrow(ufunc->get_full_name()).contains(search)) {
                        continue;
                    }

                    functions_sorted_by_call_count.push_back(ufunc);
                }

                std::sort(functions_sorted_by_call_count.begin(), functions_sorted_by_call_count.end(), [this](sdk::UFunction* a, sdk::UFunction* b) {
                    return m_called_functions[a].call_count > m_called_functions[b].call_count;
                });

                for (auto& ufunc : functions_sorted_by_call_count) {
                    if (m_ignored_recent_functions.contains(ufunc)) {
                        continue;
                    }

                    if (!this->exists(ufunc)) {
                        functions_to_cleanup.push_back(ufunc);
                        continue;
                    }

                    ImGui::PushID(ufunc);

                    utility::ScopeGuard ___{[]() {
                        ImGui::PopID();
                    }};

                    if (ImGui::Button("Ignore")) {
                        m_ignored_recent_functions.insert(ufunc);
                    }

                    ImGui::SameLine();
                    
                    const auto made = ImGui::TreeNode(utility::narrow(ufunc->get_full_name()).c_str());

                    ImGui::SameLine();
                    ImGui::Text(" (%llu)", m_called_functions[ufunc].call_count);

                    if (made) {
                        auto& data = m_called_functions[ufunc];
                        data.wants_heavy_data = true;

                        if (data.heavy_data != nullptr) {
                            // Param inspector.
                            ui_handle_struct(data.heavy_data->params.data(), ufunc);
                        }

                        ImGui::TreePop();
                    }
                }

                ImGui::TreePop();
            }

            if (!functions_to_cleanup.empty()) {
                spdlog::info("[UObjectHook] Cleaning up {} functions", functions_to_cleanup.size());

                for (auto& ufunc : functions_to_cleanup) {
                    std::erase_if(m_most_recent_functions, [ufunc](auto& it) {
                        return it == ufunc;
                    });

                    m_called_functions.erase(ufunc);
                }
            }
        }
    } else {
        ImGui::Text("Failed to hook ProcessEvent!");
    }

    ImGui::Separator();

    static std::array<char, 512> address_buffer{};
    ImGui::InputText("Address Lookup", address_buffer.data(), address_buffer.size());

    // Try-catch block around this because it's possible the user could enter invalid input
    // also hex->int conversion can throw
    try {
        auto obj = (sdk::UObject*)std::stoull(address_buffer.data(), nullptr, 16);

        if (obj != nullptr && this->exists(obj)) {
            ImGui::PushID(obj);
            if (ImGui::TreeNode(utility::narrow(obj->get_full_name()).c_str())) {
                ui_handle_object(obj);
                ImGui::TreePop();
            }

            ImGui::PopID();
        }
    } catch (...) {
        // ignore
    }
}

void UObjectHook::draw_main() {
    if (!m_motion_controller_attached_components.empty()) {
        const auto made = ImGui::TreeNode("Attached Components");

        if (made) {
            if (ImGui::Button("Detach all")) {
                m_motion_controller_attached_components.clear();

                for (auto persistent_state : m_persistent_states) {
                    if (persistent_state != nullptr) {
                        persistent_state->erase_json_file();
                    }
                }

                m_persistent_states.clear();
            }

            // make a copy because the user could press the detach button while iterating
            auto attached = decltype(m_motion_controller_attached_components){};

            if (is_stalker2_uobjecthook_guard_enabled() &&
                m_motion_controller_attached_components.size() > STALKER2_BULK_SCENE_ATTACHMENT_CAP) {
                m_stalker2_lazy_stats.tick_bulk_skips.fetch_add(
                    m_motion_controller_attached_components.size(),
                    std::memory_order_relaxed);
                ImGui::TextWrapped(
                    "Stalker2 lazy mode suppressed %zu attached-component entries. Use Detach bulk SceneComponents in UObjectHook Health if this came from an old profile.",
                    m_motion_controller_attached_components.size());
            } else if (is_stalker2_uobjecthook_guard_enabled()) {
                attached.reserve(std::min<size_t>(m_motion_controller_attached_components.size(), STALKER2_CLASS_BROWSER_OBJECT_CAP));

                for (const auto& [component, state] : m_motion_controller_attached_components) {
                    if (component == nullptr || state == nullptr || is_stalker2_bulk_scene_component(component)) {
                        m_stalker2_lazy_stats.tick_bulk_skips.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    attached.emplace(component, state);

                    if (attached.size() >= STALKER2_CLASS_BROWSER_OBJECT_CAP) {
                        break;
                    }
                }
            } else {
                attached = m_motion_controller_attached_components;
            }

            for (auto& it : attached) {
                if (!this->exists_unsafe(it.first) || it.second == nullptr) {
                    continue;
                }

                auto comp = it.first;
                std::wstring comp_name = comp->get_class()->get_fname().to_string() + L" " + comp->get_fname().to_string();

                ImGui::PushID(comp);
                if (ImGui::TreeNode(utility::narrow(comp_name).data())) {
                    ui_handle_object(comp);

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            ImGui::TreePop();
        }
    }

    const auto made2 = m_camera_attach.object != nullptr && ImGui::TreeNode("Attached Camera Object");

    if (made2) {
        if (ImGui::Button("Detach Camera")) {
            m_camera_attach.object = nullptr;
            m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};

            if (m_persistent_camera_state != nullptr) {
                m_persistent_camera_state->erase_json_file();
            }

            m_persistent_camera_state.reset();
        }

        ui_handle_object(m_camera_attach.object);

        ImGui::TreePop();
    }

    if (m_overlap_detection_actor == nullptr) {
        if (ImGui::Button("Spawn Overlapper")) {
            spawn_overlapper(0);
            spawn_overlapper(1);
        }
    } else if (!this->exists_unsafe(m_overlap_detection_actor)) {
        m_overlap_detection_actor = nullptr;
    } else {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        const auto made = ImGui::TreeNode("Overlapped Objects");

        if (made) {
            if (ImGui::Button("Destroy Overlapper")) {
                destroy_overlapper();
            }

            ImGui::SameLine();
            bool attach_all = false;
            if (ImGui::Button("Attach all")) {
                attach_all = true;
            }

            auto overlapped_components = m_overlap_detection_actor->get_overlapping_components();

            for (auto& it : overlapped_components) {
                auto comp = (sdk::USceneComponent*)it;
                if (!this->exists_unsafe(comp)) {
                    continue;
                }

                if (m_spawned_spheres.contains(comp) && m_spawned_spheres_to_components.contains(comp)) {
                    comp = m_spawned_spheres_to_components[comp];
                }

                if (attach_all){ 
                    if (!m_motion_controller_attached_components.contains(comp)) {
                        m_motion_controller_attached_components[comp] = std::make_shared<MotionControllerState>();
                    }
                }

                std::wstring comp_name = comp->get_class()->get_fname().to_string() + L" " + comp->get_fname().to_string();

                if (ImGui::TreeNode(utility::narrow(comp_name).data())) {
                    ui_handle_object(comp);
                    ImGui::TreePop();
                }
            }

            ImGui::TreePop();
        }
    }

    const auto uobject_array = sdk::FUObjectArray::get();
    const auto actual_object_count = uobject_array != nullptr ? uobject_array->get_object_count() : 0;
    ImGui::Text("Objects: %zu (%zu actual)", m_objects.size(), actual_object_count);

    if (ImGui::TreeNode("UObjectHook Health")) {
        const bool add_object_guard_unreliable = m_add_object_guard_unreliable.load(std::memory_order_relaxed);
        const bool force_scan = m_force_uobject_array_creation_scan.load(std::memory_order_relaxed);
        const auto add_object_valid_candidates = m_add_object_guard_stats.valid_candidate_calls.load(std::memory_order_relaxed);
        const auto add_object_no_candidates = m_add_object_guard_stats.no_candidate_calls.load(std::memory_order_relaxed);
        const auto add_object_unreliable_transitions = m_add_object_guard_stats.unreliable_transitions.load(std::memory_order_relaxed);

        ImGui::Text(
            "AddObject hook: %s",
            add_object_guard_unreliable ? "hooked but unreliable; fallback scan" : (m_add_object_hooked ? "hooked" : "fallback scan"));
        ImGui::Text(
            "AddObject guard: valid=%llu no_candidate=%llu unreliable_transitions=%llu",
            add_object_valid_candidates,
            add_object_no_candidates,
            add_object_unreliable_transitions);
        ImGui::Text(
            "Scan: cursor=%d/%d budget=%u full_sweep=%s force_scan=%s",
            m_uobject_array_scan_cursor,
            actual_object_count,
            m_uobject_array_scan_stats.last_budget,
            m_uobject_array_full_sweep_active ? "yes" : "no",
            force_scan ? "yes" : "no");
        ImGui::Text(
            "Scan totals: ticks=%llu scanned=%llu added=%llu rejected=%llu tombstone_skips=%llu full_sweeps=%llu",
            m_uobject_array_scan_stats.ticks,
            m_uobject_array_scan_stats.scanned,
            m_uobject_array_scan_stats.added,
            m_uobject_array_scan_stats.rejected,
            m_uobject_array_scan_stats.tombstone_skips,
            m_uobject_array_scan_stats.full_sweeps);
        ImGui::Text(
            "Persistent misses: %llu tombstones=%zu",
            m_uobject_array_scan_stats.persistent_tracking_misses,
            m_destroyed_object_tombstones.size());
        ImGui::Text(
            "UI nested resolver: attempts=%llu adopted=%llu refused=%llu cached_refusals=%llu",
            m_ui_nested_resolve_stats.attempts,
            m_ui_nested_resolve_stats.adopted,
            m_ui_nested_resolve_stats.refused,
            m_ui_nested_resolve_stats.cached_refusals);

        if (is_stalker2_uobjecthook_guard_enabled()) {
            const auto bulk_scene_attachments = get_stalker2_bulk_scene_attachment_count();
            const auto addobject_skips = m_stalker2_lazy_stats.addobject_skips.load(std::memory_order_relaxed);
            const auto persistent_bulk_skips = m_stalker2_lazy_stats.persistent_bulk_skips.load(std::memory_order_relaxed);
            const auto tick_bulk_skips = m_stalker2_lazy_stats.tick_bulk_skips.load(std::memory_order_relaxed);
            const auto persistent_path_skips = m_stalker2_lazy_stats.persistent_path_skips.load(std::memory_order_relaxed);
            const auto persistent_budget_skips = m_stalker2_lazy_stats.persistent_budget_skips.load(std::memory_order_relaxed);
            const auto class_browser_suppressed = m_stalker2_lazy_stats.class_browser_suppressed.load(std::memory_order_relaxed);
            const bool full_scan_requested = m_stalker2_uobject_full_scan_requested.load(std::memory_order_relaxed);

            ImGui::Separator();
            ImGui::Text(
                "Stalker2 Lazy Mode: on%s class_browser=%s",
                full_scan_requested ? " (full scan requested)" : "",
                m_stalker2_class_browser_enabled ? "on" : "off");
            ImGui::Text(
                "Stalker2 lazy skips: addobject=%llu persistent_bulk=%llu tick_bulk=%llu path=%llu budget=%llu class_ui=%llu",
                addobject_skips,
                persistent_bulk_skips,
                tick_bulk_skips,
                persistent_path_skips,
                persistent_budget_skips,
                class_browser_suppressed);

            if (bulk_scene_attachments > STALKER2_BULK_SCENE_ATTACHMENT_CAP) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.35f, 0.1f, 1.0f),
                    "Bulk SceneComponent attachments: %zu/%zu; ticking is suppressed",
                    bulk_scene_attachments,
                    STALKER2_BULK_SCENE_ATTACHMENT_CAP);
            } else {
                ImGui::Text(
                    "Bulk SceneComponent attachments: %zu/%zu",
                    bulk_scene_attachments,
                    STALKER2_BULK_SCENE_ATTACHMENT_CAP);
            }

            if (ImGui::Button("Detach non-persistent")) {
                const auto removed = detach_non_persistent_motion_controller_states();
                SPDLOG_INFO("[Stalker2][UObjectHook] Detached {} non-persistent motion-controller states from UI", removed);
            }

            ImGui::SameLine();

            if (ImGui::Button("Detach bulk SceneComponents")) {
                const auto removed = detach_stalker2_bulk_scene_component_states();
                SPDLOG_WARN("[Stalker2][UObjectHook] Detached {} bulk SceneComponent motion-controller states from UI", removed);
            }
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Recent Objects")) {
        for (auto& object : m_most_recent_objects) {
            if (!this->exists_unsafe(object)) {
                continue;
            }

            if (ImGui::TreeNode(utility::narrow(object->get_full_name()).data())) {
                ui_handle_object(object);
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }

    // Display common objects like things related to the player
    if (ImGui::TreeNode("Common Objects")) {
        auto engine = sdk::UGameEngine::get();
        auto world = engine != nullptr ? engine->get_world() : nullptr;

        if (world != nullptr) {
            const bool stalker2_common_root = is_stalker2_uobjecthook_guard_enabled();
            const bool require_common_root_array_member = !stalker2_common_root;

            if (ImGui::TreeNode("PlayerController")) {
                auto scope = m_path.enter_clean("Player Controller");
                auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

                if (player_controller != nullptr) {
                    try_track_object(
                        (sdk::UObjectBase*)player_controller,
                        "Common Objects/PlayerController",
                        require_common_root_array_member,
                        false);
                    ui_handle_object(player_controller);
                } else {
                    ImGui::Text("No player controller");
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Acknowledged Pawn")) {
                auto scope = m_path.enter_clean("Acknowledged Pawn");
                auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

                if (player_controller != nullptr) {
                    auto pawn = player_controller->get_acknowledged_pawn();

                    if (pawn != nullptr) {
                        try_track_object(
                            (sdk::UObjectBase*)pawn,
                            "Common Objects/Acknowledged Pawn",
                            require_common_root_array_member,
                            false);
                        ui_handle_object(pawn);
                    } else {
                        ImGui::Text("No pawn");
                    }
                } else {
                    ImGui::Text("No player controller");
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Camera Manager")) {
                auto scope = m_path.enter_clean("Camera Manager");
                auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

                if (player_controller != nullptr) {
                    auto camera_manager = player_controller->get_player_camera_manager();

                    if (camera_manager != nullptr) {
                        try_track_object(
                            (sdk::UObjectBase*)camera_manager,
                            "Common Objects/Camera Manager",
                            require_common_root_array_member,
                            false);
                        ui_handle_object((sdk::UObject*)camera_manager);
                    } else {
                        ImGui::Text("No camera manager");
                    }
                } else {
                    ImGui::Text("No player controller");
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("World")) {
                auto scope = m_path.enter_clean("World");
                try_track_object((sdk::UObjectBase*)world, "Common Objects/World", require_common_root_array_member, false);
                ui_handle_object(world);
                ImGui::TreePop();
            }
        } else {
            ImGui::Text("No world");
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Objects by class")) {
        if (is_stalker2_uobjecthook_guard_enabled()) {
            const bool full_scan_requested = m_stalker2_uobject_full_scan_requested.load(std::memory_order_relaxed);
            ImGui::TextWrapped(
                "Stalker2 lazy mode is active. Class browsing is off by default because sorting/backfilling the gameplay object set can hitch badly after level load.");

            if (full_scan_requested) {
                ImGui::Text("Full scan is running with a low per-frame budget.");
            } else if (ImGui::Button("Full scan UObjectArray (slow)")) {
                request_stalker2_uobject_full_scan();
            }

            ImGui::SameLine();

            if (!m_stalker2_class_browser_enabled && ImGui::Button("Enable class browser")) {
                m_stalker2_class_browser_enabled = true;
            }

            ImGui::Separator();

            if (!m_stalker2_class_browser_enabled) {
                m_stalker2_lazy_stats.class_browser_suppressed.fetch_add(1, std::memory_order_relaxed);
                ImGui::TextWrapped("Open Common Objects for PlayerController, Acknowledged Pawn, Camera Manager, and World. Use class browsing only when you need a full object search.");
                ImGui::TreePop();
                return;
            }
        }

        ImGui::Checkbox("Hide Default Classes", &m_hide_default_classes);

        static char filter[256]{};
        ImGui::InputText("Filter", filter, sizeof(filter));

        const bool filter_empty = std::string_view{filter}.empty();

        const auto now = std::chrono::steady_clock::now();
        bool needs_sort = true;

        if (m_sorting_task.valid()) {
            // Check if the sorting task is finished
            if (m_sorting_task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                // Do something if needed when sorting is done
                m_sorted_classes = m_sorting_task.get();
                needs_sort = true;
            } else {
                needs_sort = false;
            }
        } else {
            needs_sort = true;
        }

        if (needs_sort) {
            auto sort_classes = [this](std::vector<sdk::UClass*> classes) {
                std::sort(classes.begin(), classes.end(), [this](sdk::UClass* a, sdk::UClass* b) {
                    std::shared_lock _{m_mutex};
                    if (!m_objects.contains(a) || !m_objects.contains(b)) {
                        return false;
                    }

                    return m_meta_objects[a]->full_name < m_meta_objects[b]->full_name;
                });

                return classes;
            };

            auto unsorted_classes = std::vector<sdk::UClass*>{};

            for (auto& [c, set]: m_objects_by_class) {
                unsorted_classes.push_back(c);
            }

            // Launch sorting in a separate thread
            m_sorting_task = std::async(std::launch::async, sort_classes, unsorted_classes);
            m_last_sort_time = now;
        }

        const auto wide_filter = utility::widen(filter);
        const bool made_child = ImGui::BeginChild("Objects by class entries", ImVec2(0, 0), true, ImGuiWindowFlags_::ImGuiWindowFlags_HorizontalScrollbar);

        utility::ScopeGuard sg{[made_child]() {
            //if (made_child) {
                // well apparently BeginChild doesn't care about if it returned true or not so...
                // TODO: check this every time we update imgui?
                ImGui::EndChild();
            //}
        }};

        size_t stalker2_displayed_classes = 0;

        for (auto uclass : m_sorted_classes) {
            if (is_stalker2_uobjecthook_guard_enabled() &&
                stalker2_displayed_classes >= STALKER2_CLASS_BROWSER_CLASS_CAP) {
                ImGui::TextWrapped(
                    "Stalker2 class browser capped at %zu classes this frame. Use the filter or Full scan only if you need more.",
                    STALKER2_CLASS_BROWSER_CLASS_CAP);
                break;
            }

            const auto& objects_ref = m_objects_by_class[uclass];

            if (objects_ref.empty()) {
                continue;
            }

            if (!m_meta_objects.contains(uclass)) {
                continue;
            }

            if (objects_ref.size() == 1 && m_hide_default_classes) {
                auto first = *objects_ref.begin();

                if (m_meta_objects.contains(first)) {
                    auto fc = first != nullptr ? m_meta_objects[first]->uclass : nullptr;

                    if (fc != nullptr && m_meta_objects.contains(fc) && fc->get_class_default_object() == first) {
                        continue;
                    }
                }
            }

            const auto uclass_name = utility::narrow(m_meta_objects[uclass]->full_name);
            bool valid = true;

            if (!filter_empty) {
                valid = false;

                for (auto super = (sdk::UStruct*)uclass; super; super = super->get_super_struct()) {
                    if (auto it = m_meta_objects.find(super); it != m_meta_objects.end()) {
                        if (it->second->full_name.find(wide_filter) != std::wstring::npos) {
                            valid = true;
                            break;
                        }
                    }
                }
            }

            if (!valid) {
                continue;
            }

            if (is_stalker2_uobjecthook_guard_enabled()) {
                ++stalker2_displayed_classes;
            }

            if (ImGui::TreeNode(uclass_name.data())) {
                ui_standard_object_context_menu(uclass);

                std::vector<sdk::UObjectBase*> objects{};

                for (auto object : objects_ref) {
                    if (m_hide_default_classes) {
                        if (auto c = m_meta_objects[object]->uclass; c != nullptr && m_meta_objects.contains(c) && c->get_class_default_object() != object) {
                            objects.push_back(object);
                        }
                    } else {
                        objects.push_back(object);   
                    }
                }

                std::sort(objects.begin(), objects.end(), [this](sdk::UObjectBase* a, sdk::UObjectBase* b) {
                    return m_meta_objects[a]->full_name < m_meta_objects[b]->full_name;
                });

                if (is_stalker2_uobjecthook_guard_enabled() &&
                    objects.size() > STALKER2_CLASS_BROWSER_OBJECT_CAP) {
                    ImGui::TextWrapped(
                        "Showing first %zu/%zu objects for this Stalker2 class. Use filters or a direct address lookup for more.",
                        STALKER2_CLASS_BROWSER_OBJECT_CAP,
                        objects.size());
                    objects.resize(STALKER2_CLASS_BROWSER_OBJECT_CAP);
                }

                if (uclass->is_a(sdk::AActor::static_class())) {
                    static char component_add_name[256]{};

                    if (ImGui::InputText("Add Component Permanently", component_add_name, sizeof(component_add_name), ImGuiInputTextFlags_::ImGuiInputTextFlags_EnterReturnsTrue)) {
                        const auto component_c = sdk::find_uobject<sdk::UClass>(utility::widen(component_add_name));

                        if (component_c != nullptr) {
                            m_on_creation_add_component_jobs[uclass] = [this, component_c](sdk::UObject* object) {
                                if (!this->exists(object)) {
                                    return;
                                }

                                if (object == object->get_class()->get_class_default_object()) {
                                    return;
                                }

                                auto actor = (sdk::AActor*)object;
                                auto component = (sdk::UObject*)actor->add_component_by_class(component_c);

                                if (component != nullptr) {
                                    if (component->get_class()->is_a(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SphereComponent"))) {
                                        struct SphereRadiusParams {
                                            float radius{};
                                        };

                                        auto params = SphereRadiusParams{};
                                        params.radius = 100.f;

                                        const auto fn = component->get_class()->find_function(L"SetSphereRadius");

                                        if (fn != nullptr) {
                                            component->process_event(fn, &params);
                                        }
                                    }

                                    struct {
                                        bool hidden{false};
                                        bool propagate{true};
                                    } set_hidden_params{};

                                    const auto fn = component->get_class()->find_function(L"SetHiddenInGame");

                                    if (fn != nullptr) {
                                        component->process_event(fn, &set_hidden_params);
                                    }

                                    actor->finish_add_component(component);

                                    // Set component_add_name to empty
                                    component_add_name[0] = '\0';
                                } else {
                                    component_add_name[0] = 'e';
                                    component_add_name[1] = 'r';
                                    component_add_name[2] = 'r';
                                    component_add_name[3] = '\0';
                                }
                            };
                        } else {
                            strcpy_s(component_add_name, "Nonexistent component");
                        }
                    }
                }

                for (const auto& object : objects) {
                    const auto made = ImGui::TreeNode(utility::narrow(m_meta_objects[object]->full_name).data());

                    if (made) {
                        ui_handle_object((sdk::UObject*)object);
                        
                        ImGui::TreePop();
                    } else {
                        ui_standard_object_context_menu(object);
                    }
                }

                ImGui::TreePop();
            } else {
                ui_standard_object_context_menu(uclass);
            }
        }

        ImGui::TreePop();
    }
}

void UObjectHook::ui_standard_object_context_menu(sdk::UObjectBase* object) {
    if (ImGui::BeginPopupContextItem()) {
        auto sc = [](const std::string& text) {
            if (OpenClipboard(NULL)) {
                EmptyClipboard();
                HGLOBAL hcd = GlobalAlloc(GMEM_DDESHARE, text.size() + 1);
                char* data = (char*)GlobalLock(hcd);
                strcpy(data, text.c_str());
                GlobalUnlock(hcd);
                SetClipboardData(CF_TEXT, hcd);
                CloseClipboard();
            }
        };

        if (ImGui::Button("Copy Name")) {
            sc(utility::narrow(m_meta_objects[object]->full_name));
        }

        if (ImGui::Button("Copy Address")) {
            const auto hex = (std::stringstream{} << std::hex << (uintptr_t)object).str();
            sc(hex);
        }

        ImGui::EndPopup();
    }
}

void UObjectHook::ui_handle_object(sdk::UObject* object) {
    if (object == nullptr) {
        ImGui::Text("nullptr");
        return;
    }

    if (!this->exists_unsafe(object)) {
        try_track_reachable_ui_object(nullptr, object, "ui direct object");
    }

    if (!this->exists_unsafe(object)) {
        ImGui::Text("Invalid object");
        return;
    }

    ui_standard_object_context_menu(object);

    const auto uclass = object->get_class();

    if (uclass == nullptr) {
        ImGui::Text("null class");
        return;
    }

    if (!this->exists_unsafe(uclass)) {
        try_track_object((sdk::UObjectBase*)uclass, "ui object class", false, false);
    }

    if (!this->exists_unsafe(uclass)) {
        ImGui::Text("Invalid class");
        return;
    }

    if (object->is_a(sdk::UClass::static_class())) {
        if (ImGui::TreeNode("Default Object")) {
            auto def = ((sdk::UClass*)object)->get_class_default_object();

            if (def != nullptr) {
                ui_handle_reachable_object(object, def, "ui default object");
            } else {
                ImGui::Text("Null default object");
            }

            ImGui::TreePop();
        }
    }

    ImGui::Text("%s", utility::narrow(object->get_full_name()).data());

    if (ImGui::TreeNode("Outer")) {
        auto outer_scope = m_path.enter("Outer");
        ui_handle_reachable_object(object, object->get_outer(), "ui outer");
        ImGui::TreePop();
    }

    static const auto material_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.MaterialInterface");

    if (uclass->is_a(material_t)) {
        ui_handle_material_interface(object);
    }

    static const auto widget_component_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/UMG.WidgetComponent");

    if (uclass->is_a(widget_component_t)) {
        if (ImGui::Button("Set to Screen Space")) {
            static const auto set_widget_space_fn = uclass->find_function(L"SetWidgetSpace");

            if (set_widget_space_fn != nullptr) {
                struct {
                    uint32_t space{1};
                } params{};

                object->process_event(set_widget_space_fn, &params);
            }
        }
    }

    if (uclass->is_a(sdk::UActorComponent::static_class())) {
        /*if (ImGui::Button("Destroy Component")) {
            auto comp = (sdk::UActorComponent*)object;

            comp->destroy_component();
        }*/
    }

    if (uclass->is_a(sdk::USceneComponent::static_class())) {
        ui_handle_scene_component((sdk::USceneComponent*)object);
    }

    if (uclass->is_a(sdk::AActor::static_class())) {
        ui_handle_actor(object);
    }

    ui_handle_struct(object, uclass);
}

void UObjectHook::ui_handle_scene_component(sdk::USceneComponent* comp) {
    if (comp == nullptr) {
        return;
    }

    bool attached = m_motion_controller_attached_components.contains(comp);

    if (attached) {
        if (ImGui::Button("Detach")) {
            m_motion_controller_attached_components.erase(comp);

            auto existing = std::find_if(m_persistent_states.begin(), m_persistent_states.end(), [&](const auto& state2) {
                return state2 != nullptr && state2->path.resolve() == comp;
            });

            if (existing != m_persistent_states.end()) {
                (*existing)->erase_json_file();
                m_persistent_states.erase(existing);
            }
        }

        if (m_motion_controller_attached_components.contains(comp)) {
            ImGui::SameLine();
            auto& state = m_motion_controller_attached_components[comp];

            if (ImGui::Checkbox("Adjust", &state->adjusting)) {
                if (state->adjusting && m_overlap_detection_actor == nullptr) {
                    VR::get()->set_aim_allowed(false);
                    g_framework->set_draw_ui(false);
                }
            }

            ImGui::SameLine();

            if (ImGui::Checkbox("Permanent Change", &state->permanent)) {
                // Locate the existing persistent state if it exists
                auto existing = std::find_if(m_persistent_states.begin(), m_persistent_states.end(), [&](const auto& state2) {
                    return state2 != nullptr && state2->path.resolve() == comp;
                });

                if (existing != m_persistent_states.end()) {
                    (*existing)->state.permanent = state->permanent;
                }
            }

            auto existing = std::find_if(m_persistent_states.begin(), m_persistent_states.end(), [&](const auto& state2) {
                return state2 != nullptr && state2->path.resolve() == comp;
            });

            // Finetuning of the controller rotation offset
            // Convert to pitch/yaw/roll first.
            auto euler = utility::math::euler_angles_from_steamvr(state->rotation_offset);
            if (ImGui::DragFloat3("RotationOffset", &euler.x, 0.01f)) {
                // Convert back to quaternion
                state->rotation_offset = glm::quat{glm::yawPitchRoll(-euler.y, euler.x, -euler.z)};

                if (existing != m_persistent_states.end()) {
                    (*existing)->state.rotation_offset = state->rotation_offset;
                }
            }

            // Finetuning of the controller position offset.
            if (ImGui::DragFloat3("PositionOffset", &state->location_offset.x, 0.01f)) {
                if (existing != m_persistent_states.end()) {
                    (*existing)->state.location_offset = state->location_offset;
                }
            }

            auto save_state_logic = [&](const std::vector<std::string>& path) {
                auto json = serialize_mc_state(path, state);

                // Concat the entire path together and hash it to get a unique name
                std::string concat_path{};
                for (const auto& p : path) {
                    concat_path += p;
                }

                const auto hash_str = std::to_string(utility::hash(concat_path)) + "_mc_state.json";
                auto wanted_path = UObjectHook::get_persistent_dir() / hash_str;

                // Use the one this was originally loaded from instead.
                if (existing != m_persistent_states.end() && (*existing)->path_to_json.has_value()) {
                    wanted_path = (*existing)->path_to_json.value();
                }

                // Create dir if necessary
                try {
                    std::filesystem::create_directories(wanted_path.parent_path());

                    if (std::filesystem::exists(wanted_path.parent_path())) {
                        std::ofstream file{wanted_path};
                        file << json.dump(4);
                        file.close();

                        m_persistent_states = deserialize_all_mc_states();
                    }
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("[UObjectHook] Failed to save motion controller state: {}", e.what());
                } catch (...) {
                    SPDLOG_ERROR("[UObjectHook] Failed to save motion controller state");
                }
            };

            // Save state stuff
            // First one is for checking whether we already have an existing persistent state
            // with its own path.
            if (existing != m_persistent_states.end()) {
                if (ImGui::Button("Save state")) {
                    save_state_logic((*existing)->path.path());
                }
            } else if (m_path.has_valid_base()) {
                if (ImGui::Button("Save state")) {
                    save_state_logic(m_path.path());
                }
            } else {
                if (auto path = try_get_path(comp); path.has_value()) {
                    if (ImGui::Button("Save state")) {
                        save_state_logic(path->path());
                    }
                } else {
                    ImGui::Text("Can't save, did not start from a valid base or none of the allowed bases can reach this component");
                }
            }
        }
    } else {
        if (m_camera_attach.object != comp) {
            if (ImGui::Button("Attach left")) {
                m_motion_controller_attached_components[comp] = std::make_shared<MotionControllerState>();
                m_motion_controller_attached_components[comp]->hand = 0;
            }

            ImGui::SameLine();

            if (ImGui::Button("Attach right")) {
                m_motion_controller_attached_components[comp] = std::make_shared<MotionControllerState>();
                m_motion_controller_attached_components[comp]->hand = 1;
            }

            ImGui::SameLine();

            if (ImGui::Button("Attach HMD")) {
                m_motion_controller_attached_components[comp] = std::make_shared<MotionControllerState>();
                m_motion_controller_attached_components[comp]->hand = 2;
            }

            if (ImGui::Button("Attach Camera to")) {
                m_camera_attach.object = comp;
                m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};
            }

            ImGui::SameLine();

            if (ImGui::Button("Attach Camera to (Relative)")) {
                m_camera_attach.object = comp;
                m_camera_attach.offset = glm::vec3{0.0f, 0.0f, m_last_camera_location.z - comp->get_world_location().z};
            }
        } else {
            if (ImGui::Button("Detach")) {
                m_camera_attach.object = nullptr;
                m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};

                if (m_persistent_camera_state != nullptr) {
                    m_persistent_camera_state->erase_json_file();
                }

                m_persistent_camera_state.reset();
            }

            ImGui::SameLine();

            if (m_persistent_camera_state != nullptr && m_persistent_camera_state->path.resolve() == comp) {
                if (ImGui::Button("Save state")) {
                    save_camera_state(m_persistent_camera_state->path.path());
                }
            } else if (m_path.has_valid_base()) {
                if (ImGui::Button("Save state")) {
                    save_camera_state(m_path.path());
                }
            } else if (auto path = try_get_path(comp); path.has_value()) {
                if (ImGui::Button("Save state")) {
                    save_camera_state(path->path());
                }
            } else {
                ImGui::Text("Can't save, did not start from a valid base or none of the allowed bases can reach this component");
            }

            if (ImGui::DragFloat3("Camera Offset", &m_camera_attach.offset.x, 0.1f)) {
                if (m_persistent_camera_state != nullptr) {
                    m_persistent_camera_state->offset = m_camera_attach.offset;
                }
            }
        }
    }

    const auto prim_comp_t = sdk::UPrimitiveComponent::static_class();
    auto prim_comp = (sdk::UPrimitiveComponent*)comp;

    bool visible = comp->is_a(prim_comp_t) ? prim_comp->is_rendering_in_main_pass() : comp->is_visible();
    bool legacy_visible = comp->is_visible();

    auto visible_checkbox = ImGui::Checkbox("Visible", &visible);
    ImGui::SameLine();

    const auto legacy_visible_changed = ImGui::Checkbox("Legacy", &legacy_visible);
    visible_checkbox |= legacy_visible_changed;

    if (visible_checkbox) {
        if (legacy_visible_changed) {
            visible = legacy_visible;
        }

        if (comp->is_a(prim_comp_t)) {
            prim_comp->set_overall_visibility(visible, legacy_visible_changed || (visible == true && !legacy_visible));
        } else {
            comp->set_visibility(visible, false);
        }

        if (visible) {
            // Check if we have a persistent property for this component
            std::shared_ptr<PersistentProperties> props{};

            for (const auto& existing_prop : m_persistent_properties) {
                if (existing_prop->path.resolve() == comp) {
                    props = existing_prop;
                    break;
                }
            }

            if (props != nullptr && props->hide) {
                props->hide = false;
                props->save_to_file();
            }
        }

        if (legacy_visible) {
            // Check if we have a persistent property for this component
            std::shared_ptr<PersistentProperties> props{};

            for (const auto& existing_prop : m_persistent_properties) {
                if (existing_prop->path.resolve() == comp) {
                    props = existing_prop;
                    break;
                }
            }

            if (props != nullptr && props->hide_legacy) {
                props->hide_legacy = false;
                props->save_to_file();
            }
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Save Visibility State")) {
        std::shared_ptr<PersistentProperties> props{};

        // Find existing one if possible
        for (const auto& existing_prop : m_persistent_properties) {
            if (existing_prop->path.resolve() == comp) {
                props = existing_prop;
                break;
            }
        }

        // Add new one if necessary
        if (props == nullptr) {
            if (m_path.has_valid_base()) {
                props = std::make_shared<PersistentProperties>();
                props->path = m_path;
            } else if (auto path = try_get_path(comp); path.has_value()) {
                props = std::make_shared<PersistentProperties>();
                props->path = path.value();
            } else {
                SPDLOG_ERROR("[UObjectHook] Can't save visibility state, did not start from a valid base or none of the allowed bases can reach this component");
                return;
            }
            
            if (props != nullptr) {
                m_persistent_properties.push_back(props);
            }
        }

        if (props != nullptr) {
            props->hide = !visible;
            props->hide_legacy = !legacy_visible;
            props->save_to_file();
        }
    }

    if (ImGui::TreeNode("Sockets")) {
        const auto socket_names = comp->get_all_socket_names();

        for (auto& name : socket_names) {
            //ImGui::Text("%s", utility::narrow(name.to_string()).data());
            if (ImGui::TreeNode(utility::narrow(name.to_string()).data())) {
                auto location = comp->get_socket_location(name.to_string());
                auto rotation = comp->get_socket_rotation(name.to_string());

                if (ImGui::DragFloat3("Location", &location.x, 0.1f)) {
                    //comp->set_socket_location(name, location);
                }

                if (ImGui::DragFloat3("Rotation", &rotation.x, 0.1f)) {
                    //comp->set_socket_rotation(name, rotation);
                }

                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

std::optional<UObjectHook::StatePath> UObjectHook::try_get_path(sdk::UObject* target) const {
    const auto component_name = utility::narrow(target->get_class()->get_fname().to_string() + L" " + target->get_fname().to_string());

    // Check if any of our bases can reach this component through their components list.
    for (const auto& allowed_base : s_allowed_bases) {
        const auto possible_path = std::vector<std::string>{allowed_base, "Components", component_name};
        const auto path = StatePath{possible_path};
        const auto resolved = path.resolve();

        if (resolved == target) {
            return path;
        }
    }

    // Try to look through the objects' properties instead now.
    for (const auto& allowed_base : s_allowed_bases) {
        const auto base_obj = StatePath{{allowed_base}}.resolve_base_object();

        if (base_obj == nullptr) {
            continue;
        }

        for (auto field = base_obj->get_class()->get_child_properties(); field != nullptr; field = field->get_next()) {
            if (field->get_class()->get_name().to_string() != L"ObjectProperty") {
                continue;
            }

            const auto prop = (sdk::FObjectProperty*)field;
            const auto obj_ptr = prop->get_data<sdk::UObject*>(base_obj);

            if (obj_ptr == nullptr || *obj_ptr == nullptr) {
                continue;
            }

            const auto obj = *obj_ptr;

            if (obj == target) {
                const auto possible_path = std::vector<std::string>{allowed_base, "Properties", utility::narrow(field->get_field_name().to_string())};
                const auto path = StatePath{possible_path};
                const auto resolved = path.resolve();

                if (resolved == target) {
                    return path;
                }

                break;
            }

            // Traverse that object's components now and see if we can find it there.
            if (obj->get_class()->is_a(sdk::AActor::static_class())) {
                const auto actor = (sdk::AActor*)*obj_ptr;

                for (auto actor_comp : actor->get_all_components()) {
                    if (actor_comp != target) {
                        continue;
                    }

                    const auto possible_path = std::vector<std::string>{allowed_base, "Properties", utility::narrow(field->get_field_name().to_string()), "Components", component_name};
                    const auto path = StatePath{possible_path};
                    const auto resolved = path.resolve();

                    if (resolved == target) {
                        return path;
                    }
                }
            }
        }
    }

    return std::nullopt;
}

void UObjectHook::ui_handle_material_interface(sdk::UObject* object) {
    if (object == nullptr) {
        return;
    }

    const auto uclass = object->get_class();

    if (uclass == nullptr) {
        return;
    }

    if (ImGui::Button("Apply to all actors")) {
        static const auto mesh_component_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.StaticMeshComponent");
        static const auto create_dynamic_mat = mesh_component_t->find_function(L"CreateDynamicMaterialInstance");
        static const auto set_material_fn = mesh_component_t->find_function(L"SetMaterial");
        static const auto get_num_materials_fn = mesh_component_t->find_function(L"GetNumMaterials");

        //const auto actors = m_objects_by_class[sdk::AActor::static_class()];
        const auto components = m_objects_by_class[mesh_component_t];

        //auto components = actor->get_all_components();

        for (auto comp_obj : components) {
            auto comp = (sdk::UActorComponent*)comp_obj;
            if (comp->is_a(mesh_component_t)) {
                if (comp == comp->get_class()->get_class_default_object()) {
                    continue;
                }

                auto owner = comp->get_owner();

                if (owner == nullptr || owner->get_class()->get_class_default_object() == owner) {
                    continue;
                }

                struct {
                    int32_t num{};
                } get_num_materials_params{};

                comp->process_event(get_num_materials_fn, &get_num_materials_params);

                for (int i = 0; i < get_num_materials_params.num; ++i) {
                    GameThreadWorker::get().enqueue([this, i, object, comp]() {
                        if (!this->exists(comp) || !this->exists(object)) {
                            return;
                        }

                        /*struct {
                            int32_t index{};
                            sdk::UObject* material{};
                        } params{};

                        params.index = i;
                        params.material = object;

                        comp->process_event(set_material_fn, &params);*/

                        struct {
                            int32_t index{};
                            sdk::UObject* material{};
                            sdk::FName name{L"None"};
                            sdk::UObject* ret{};
                        } params{};

                        params.index = i;
                        params.material = object;

                        comp->process_event(create_dynamic_mat, &params);

                        if (params.ret != nullptr && object->get_full_name().find(L"GizmoMaterial") != std::wstring::npos) {
                            const auto c = params.ret->get_class();
                            static const auto set_vector_param_fn = c->find_function(L"SetVectorParameterValue");

                            struct {
                                sdk::FName name{L"GizmoColor"};
                                glm::vec4 color{};
                            } set_vector_param_params{};

                            set_vector_param_params.color.x = 1.0f;
                            set_vector_param_params.color.y = 0.0f;
                            set_vector_param_params.color.z = 0.0f;
                            set_vector_param_params.color.w = 1.0f;

                            params.ret->process_event(set_vector_param_fn, &set_vector_param_params);
                        }

                        if (params.ret != nullptr && object->get_full_name().find(L"BasicShapeMaterial") != std::wstring::npos) {
                            const auto c = params.ret->get_class();
                            static const auto set_vector_param_fn = c->find_function(L"SetVectorParameterValue");

                            struct {
                                sdk::FName name{L"Color"};
                                glm::vec4 color{};
                            } set_vector_param_params{};

                            set_vector_param_params.color.x = 1.0f;
                            set_vector_param_params.color.y = 0.0f;
                            set_vector_param_params.color.z = 0.0f;
                            set_vector_param_params.color.w = 1.0f;

                            params.ret->process_event(set_vector_param_fn, &set_vector_param_params);
                        }
                    });
                }
            }
        }
    }
}

void UObjectHook::ui_handle_actor(sdk::UObject* object) {
    if (object == nullptr) {
        return;
    }

    const auto uclass = object->get_class();

    if (uclass == nullptr) {
        return;
    }

    auto actor = (sdk::AActor*)object;

    if (m_camera_attach.object != object ){
        if (ImGui::Button("Attach Camera to")) {
            m_camera_attach.object = object;
            m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};
        }

        ImGui::SameLine();

        if (ImGui::Button("Attach Camera to (Relative)")) {
            m_camera_attach.object = object;
            m_camera_attach.offset = glm::vec3{0.0f, 0.0f, m_last_camera_location.z - actor->get_actor_location().z};
        }
    } else {
        if (ImGui::Button("Detach")) {
            m_camera_attach.object = nullptr;
            m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};

            if (m_persistent_camera_state != nullptr) {
                m_persistent_camera_state->erase_json_file();
            }

            m_persistent_camera_state.reset();
        }

        if (m_persistent_camera_state != nullptr && m_persistent_camera_state->path.resolve() == object) {
            if (ImGui::Button("Save state")) {
                save_camera_state(m_persistent_camera_state->path.path());
            }
        } else if (m_path.has_valid_base()) {
            if (ImGui::Button("Save state")) {
                save_camera_state(m_path.path());
            }
        } else if (auto path = try_get_path(object); path.has_value()) {
            if (ImGui::Button("Save state")) {
                save_camera_state(path->path());
            }
        } else {
            ImGui::Text("Can't save, did not start from a valid base or none of the allowed bases can reach this object");
        }

        if (ImGui::DragFloat3("Camera Offset", &m_camera_attach.offset.x, 0.1f)) {
            if (m_persistent_camera_state != nullptr) {
                m_persistent_camera_state->offset = m_camera_attach.offset;
            }
        }
    }

    static char component_add_name[256]{};

    if (ImGui::InputText("Add Component", component_add_name, sizeof(component_add_name), ImGuiInputTextFlags_::ImGuiInputTextFlags_EnterReturnsTrue)) {
        const auto component_c = sdk::find_uobject<sdk::UClass>(utility::widen(component_add_name));

        if (component_c != nullptr) {
            GameThreadWorker::get().enqueue([=, this]() {
                if (!this->exists(object)) {
                    return;
                }

                auto component = (sdk::UObject*)actor->add_component_by_class(component_c);

                if (component != nullptr) {
                    if (component->get_class()->is_a(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SphereComponent"))) {
                        struct SphereRadiusParams {
                            float radius{};
                        };

                        auto params = SphereRadiusParams{};
                        params.radius = 100.f;

                        const auto fn = component->get_class()->find_function(L"SetSphereRadius");

                        if (fn != nullptr) {
                            component->process_event(fn, &params);
                        }
                    }

                    struct {
                        bool hidden{false};
                        bool propagate{true};
                    } set_hidden_params{};

                    const auto fn = component->get_class()->find_function(L"SetHiddenInGame");

                    if (fn != nullptr) {
                        component->process_event(fn, &set_hidden_params);
                    }

                    actor->finish_add_component(component);

                    // Set component_add_name to empty
                    component_add_name[0] = '\0';
                } else {
                    component_add_name[0] = 'e';
                    component_add_name[1] = 'r';
                    component_add_name[2] = 'r';
                    component_add_name[3] = '\0';
                }
            });
        } else {
            strcpy_s(component_add_name, "Nonexistent component");
        }
    }

    if (ImGui::TreeNode("Components")) {
        auto scope = m_path.enter("Components");
        auto components = actor->get_all_components();

        std::sort(components.begin(), components.end(), [](sdk::UObject* a, sdk::UObject* b) {
            return a->get_full_name() < b->get_full_name();
        });

        for (auto comp : components) {
            auto comp_obj = (sdk::UObject*)comp;

            ImGui::PushID(comp_obj);
            // not using full_name because its HUGE
            std::wstring comp_name = comp->get_class()->get_fname().to_string() + L" " + comp->get_fname().to_string();
            const auto made = ImGui::TreeNode(utility::narrow(comp_name).data());

            if (made) {
                auto scope2 = m_path.enter(utility::narrow(comp_name));
                ui_handle_reachable_object(object, comp_obj, "ui actor component");
                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        ImGui::TreePop();
    }
}

void UObjectHook::ui_handle_functions(void* object, sdk::UStruct* uclass) {
    if (uclass == nullptr) {
        return;
    }

    const bool is_real_object = object != nullptr && m_objects.contains((sdk::UObject*)object);
    auto object_real = (sdk::UObject*)object;

    std::vector<sdk::UFunction*> sorted_functions{};
    static const auto ufunction_t = sdk::UFunction::static_class();

    for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
        auto funcs = super->get_children();

        for (auto func = funcs; func != nullptr; func = func->get_next()) {
            if (func->get_class()->is_a(ufunction_t)) {
                sorted_functions.push_back((sdk::UFunction*)func);
            }
        }
    }

    std::sort(sorted_functions.begin(), sorted_functions.end(), [](sdk::UFunction* a, sdk::UFunction* b) {
        return a->get_fname().to_string() < b->get_fname().to_string();
    });

    for (auto func : sorted_functions) {
        ImGui::PushID((void*)func);

        utility::ScopeGuard pop_guard{[]() {
            ImGui::PopID();
        }};

        const auto made = ImGui::TreeNode(utility::narrow(func->get_fname().to_string()).data());

        ui_standard_object_context_menu(func);

        if (m_called_functions.contains(func)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "[Called]");
        }

        if (is_real_object && made) {
            auto parameters = func->get_child_properties();

            if (parameters == nullptr || (parameters->get_next() == nullptr && parameters->get_field_name().to_string() == L"ReturnValue")) {
                if (ImGui::Button("Call")) {
                    struct {
                        char poop[1024]{};
                    } params{};

                    object_real->process_event(func, &params);
                }
            } else if (parameters->get_next() == nullptr) {
                switch (utility::hash(utility::narrow(parameters->get_class()->get_name().to_string()))) {
                case "BoolProperty"_fnv:
                    {
                        if (ImGui::Button("Enable")) {
                            struct {
                                bool enabled{true};
                                char padding[0x10];
                            } params{};

                            object_real->process_event(func, &params);
                        }

                        ImGui::SameLine();

                        if (ImGui::Button("Disable")) {
                            struct {
                                bool enabled{false};
                                char padding[0x10];
                            } params{};

                            object_real->process_event(func, &params);
                        }
                    }
                    break;
                case "StrProperty"_fnv:
                {
                    if (ImGui::Button("Call")) {
                        struct {
                            sdk::TArrayLite<wchar_t> str{};
                            char padding[0x10];
                        } params{};

                        params.str.data = (wchar_t*)L"Hello world!";
                        params.str.count = wcslen(params.str.data);
                        params.str.capacity = params.str.count + 1;

                        object_real->process_event(func, &params);
                    }
                }
                    break;
                case "UInt32Property"_fnv:
                case "IntProperty"_fnv:
                case "EnumProperty"_fnv:
                    {
                        static int value = 0;
                        ImGui::InputInt("Value", &value);

                        if (ImGui::Button("Call")) {
                            struct {
                                int value{};
                                char padding[0x10];
                            } params{};

                            params.value = value;

                            object_real->process_event(func, &params);
                        }
                    }
                    break;

                default:
                    break;
                };
            }

            for (auto param = parameters; param != nullptr; param = param->get_next()) {
                const auto cname = utility::narrow(param->get_class()->get_name().to_string());
                ImGui::Text("%s %s", cname.data(), utility::narrow(param->get_field_name().to_string()).data());

                if (cname.contains("Property")) {
                    const auto prop = (sdk::FProperty*)param;

                    if (prop->is_out_param()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "[Out]");
                    }

                    // Display full name of StructProperty
                    if (cname == "StructProperty") {
                        const auto prop = (sdk::FStructProperty*)param;
                        const auto s = prop->get_struct();

                        if (s != nullptr) {
                            const auto struct_name = utility::narrow(s->get_full_name());
                            constexpr auto r = (float)78.0f / 255.0f;
                            constexpr auto g = (float)201.0f / 255.0f;
                            constexpr auto b = (float)176.0f / 255.0f;

                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4{r, g, b, 1.0f}, "[%s]", struct_name.data());
                        }
                    }
                }
            }

            ImGui::TreePop();
        }
    }
}

void UObjectHook::ui_handle_properties(void* object, sdk::UStruct* uclass) {
    auto previous_path = m_path;

    auto scope = m_path.enter("Properties");

    if (uclass == nullptr) {
        return;
    }

    const bool is_real_object = object != nullptr && m_objects.contains((sdk::UObject*)object);

    std::vector<sdk::FField*> sorted_fields{};

    for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
        auto props = super->get_child_properties();

        for (auto prop = props; prop != nullptr; prop = prop->get_next()) {
            sorted_fields.push_back(prop);
        }
    }

    std::sort(sorted_fields.begin(), sorted_fields.end(), [](sdk::FField* a, sdk::FField* b) {
        return a->get_field_name().to_string() < b->get_field_name().to_string();
    });

    for (auto prop : sorted_fields) {
        auto propc = prop->get_class();
        const auto propc_type = propc->get_name().to_string();

        if (object == nullptr) {
            const auto name = utility::narrow(propc->get_name().to_string());
            const auto field_name = utility::narrow(prop->get_field_name().to_string());
            ImGui::Text("%s %s", name.data(), field_name.data());

            continue;
        }

        auto hash_type = utility::hash(propc_type);

        if (hash_type == L"EnumProperty"_fnv) {
            auto enum_prop = (sdk::FEnumProperty*)prop;
            if (auto numeric_prop = (sdk::FProperty*)enum_prop->get_underlying_prop(); numeric_prop != nullptr) try {
                if (auto nc = numeric_prop->get_class(); nc != nullptr) {
                    const auto nc_name = nc->get_name().to_string_no_numbers(); // Calling this variant so we don't cause a crash if the name is bad.

                    // This check is just in-case we get handed back some bad memory.
                    if (nc_name.contains(L"Property")) {
                        hash_type = utility::hash(nc_name);
                    }
                }
            } catch (...) {
                // Can happen because we haven't rigorously mapped out FEnumProperty yet for older UE versions.
            }
        }

        // Right-click lambda for supported properties, usually for saving.
        auto display_context = [&](auto value) {
            if (!ImGui::BeginPopupContextItem()) {
                return;
            }

            if (!previous_path.has_valid_base()) {
                ImGui::Text("Can't save, did not start from a valid base");
                ImGui::EndPopup();
                return;
            }

            auto save_logic = [&](bool unsave = false) {
                const auto field_name = prop->get_field_name().to_string();
                std::shared_ptr<PersistentProperties> props{};

                // Find existing one if possible
                for (const auto& existing_prop : m_persistent_properties) {
                    if (existing_prop->path.resolve() == object) {
                        props = existing_prop;
                        break;
                    }
                }

                // Add new one if necessary
                if (props == nullptr) {
                    props = std::make_shared<PersistentProperties>();
                    props->path = StatePath{previous_path.path()};
                    m_persistent_properties.push_back(props);
                }

                // Add property to list if needed
                std::shared_ptr<PersistentProperties::PropertyState> state{};

                for (const auto& existing_state : props->properties) {
                    if (existing_state->name == prop->get_field_name().to_string()) {
                        state = existing_state;
                        break;
                    }
                }

                // Add new one if necessary
                if (state == nullptr) {
                    state = std::make_shared<PersistentProperties::PropertyState>();
                    state->name = prop->get_field_name().to_string();
                    props->properties.push_back(state);
                }

                memcpy(&state->data, &value, sizeof(value));

                if (unsave) {
                    props->properties.erase(
                        std::remove(props->properties.begin(), props->properties.end(), state), 
                        props->properties.end()
                    );
                }
                
                // Concat the entire path together and hash it to get a unique name
                std::string concat_path{};
                for (const auto& p : previous_path.path()) {
                    concat_path += p;
                }

                const auto hash_str = std::to_string(utility::hash(concat_path)) + "_props.json";
                auto wanted_path = UObjectHook::get_persistent_dir() / hash_str;

                if (props->path_to_json.has_value()) {
                    wanted_path = props->path_to_json.value();
                }

                // Create dir if necessary
                try {
                    std::filesystem::create_directories(wanted_path.parent_path());

                    if (props->properties.empty()) {
                        // Delete the file if it exists. Happens if we unsave.
                        if (std::filesystem::exists(wanted_path)) {
                            std::filesystem::remove(wanted_path);
                        }

                        // Delete the property entry from m_peristent_properties.
                        m_persistent_properties.erase(
                            std::remove(m_persistent_properties.begin(), m_persistent_properties.end(), props), 
                            m_persistent_properties.end()
                        );

                        return;
                    }

                    props->save_to_file(wanted_path);
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("[UObjectHook] Failed to save persistent properties: {}", e.what());
                } catch (...) {
                    SPDLOG_ERROR("[UObjectHook] Failed to save persistent properties");
                }
            };

            if (ImGui::Button("Save Property")) {
                save_logic();
            }

            if (ImGui::Button("Unsave Property")) {
                save_logic(true);
            }

            ImGui::EndPopup();
        };

        switch (hash_type) {
        case L"FloatProperty"_fnv:
            {
                auto& value = *(float*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());
                ImGui::DragFloat(utility::narrow(prop->get_field_name().to_string()).data(), &value, 0.01f);
                display_context(value);
            }
            break;
        case L"DoubleProperty"_fnv:
            {
                auto& value = *(double*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());
                float casted_value = (float)value;
                if (ImGui::DragFloat(utility::narrow(prop->get_field_name().to_string()).data(), (float*)&casted_value, 0.01f)) {
                    value = (double)casted_value;
                }
                display_context(value);
            }
            break;
        case L"UInt16Property"_fnv:
            {
                auto& value = *(uint16_t*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());
                int converted = (int)value;
                if (ImGui::SliderInt(utility::narrow(prop->get_field_name().to_string()).data(), (int*)&converted, 0, 65535)) {
                    value = (uint16_t)converted;
                }
                display_context(value);
            }
            break;
        case L"UInt32Property"_fnv:
        case L"IntProperty"_fnv:
            {
                auto& value = *(int32_t*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());
                ImGui::DragInt(utility::narrow(prop->get_field_name().to_string()).data(), &value, 1);
                display_context(value);
            }
            break;
        case L"UInt64Property"_fnv:
            {
                auto& value = *(uint64_t*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());
                ImGui::DragScalar(utility::narrow(prop->get_field_name().to_string()).data(), ImGuiDataType_U64, &value, 1);
                display_context(value);
            }
            break;
        case L"BoolProperty"_fnv:
            {
                auto boolprop = (sdk::FBoolProperty*)prop;
                auto value = boolprop->get_value_from_object(object);
                if (ImGui::Checkbox(utility::narrow(prop->get_field_name().to_string()).data(), &value)) {
                    boolprop->set_value_in_object(object, value);
                }
                display_context(value);
            }
            break;
        case L"ByteProperty"_fnv:
            {
                auto& value = *(uint8_t*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());
                int converted = (int)value;
                if (ImGui::SliderInt(utility::narrow(prop->get_field_name().to_string()).data(), &converted, 0, 255)) {
                    value = (uint8_t)converted;
                }
                display_context(value);
            }
            break;
        case L"InterfaceProperty"_fnv:
        case L"ObjectProperty"_fnv:
        case L"ClassProperty"_fnv:
            {
                auto& value = *(sdk::UObject**)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());
                
                if (ImGui::TreeNode(utility::narrow(prop->get_field_name().to_string()).data())) {
                    auto scope2 = m_path.enter(utility::narrow(prop->get_field_name().to_string()));
                    auto parent_object = (sdk::UObject*)object;

                    if (!exists(parent_object)) {
                        parent_object = nullptr;
                    }

                    ui_handle_reachable_object(parent_object, value, "ui object property");
                    ImGui::TreePop();
                }
            }
            break;
        case L"StructProperty"_fnv:
            {
                void* addr = (void*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());

                const auto made = ImGui::TreeNode(utility::narrow(prop->get_field_name().to_string()).data());

                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::Button("Copy Address")) {
                        const auto hex = (std::stringstream{} << std::hex << (uintptr_t)addr).str();

                        if (OpenClipboard(NULL)) {
                            EmptyClipboard();
                            HGLOBAL hcd = GlobalAlloc(GMEM_DDESHARE, hex.size() + 1);
                            char* data = (char*)GlobalLock(hcd);
                            strcpy(data, hex.c_str());
                            GlobalUnlock(hcd);
                            SetClipboardData(CF_TEXT, hcd);
                            CloseClipboard();
                        }
                    }

                    ImGui::EndPopup();
                }

                if (made) {
                    auto scope2 = m_path.enter(utility::narrow(prop->get_field_name().to_string()));
                    ui_handle_struct(addr, ((sdk::FStructProperty*)prop)->get_struct());
                    ImGui::TreePop();
                }
            }
            break;
        case L"Function"_fnv:
            break;
        case L"ArrayProperty"_fnv:
            if (ImGui::TreeNode(utility::narrow(prop->get_field_name().to_string()).data())) {
                auto scope2 = m_path.enter(utility::narrow(prop->get_field_name().to_string()));
                ui_handle_array_property(object, (sdk::FArrayProperty*)prop);
                ImGui::TreePop();
            }

            break;
        case L"NameProperty"_fnv:
            {
                const auto& value = *(sdk::FName*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());
                const auto wstr = value.to_string();
                const auto str = utility::narrow(wstr);

                ImGui::Text("%s: ", utility::narrow(prop->get_field_name().to_string()).data());
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4{3.0f / 255.0f, 232.0f / 255.0f, 252.0f / 255.0f, 1.0f}, "%s", str.data());
            }
            break;
        case L"StrProperty"_fnv:
        {
            using FString = sdk::TArray<wchar_t>;
            const auto& value = *(FString*)((uintptr_t)object + ((sdk::FProperty*)prop)->get_offset());

            if (value.data != nullptr && value.count > 0) {
                const auto str = std::wstring_view{value.data, (size_t)value.count};
                const auto narrow_str = utility::narrow(str);

                ImGui::Text("%s: ", utility::narrow(prop->get_field_name().to_string()).data());
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4{3.0f / 255.0f, 232.0f / 255.0f, 252.0f / 255.0f, 1.0f}, "%s", narrow_str.data());
            } else {
                ImGui::Text("%s: ", utility::narrow(prop->get_field_name().to_string()).data());
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4{3.0f / 255.0f, 232.0f / 255.0f, 252.0f / 255.0f, 1.0f}, "[empty string]");
            }
            break;
        }
        default:
            {
                const auto name = utility::narrow(propc->get_name().to_string());
                const auto field_name = utility::narrow(prop->get_field_name().to_string());
                ImGui::Text("%s %s", name.data(), field_name.data());
            }
            break;
        };
    }
}

void UObjectHook::ui_handle_array_property(void* addr, sdk::FArrayProperty* prop) {
    if (addr == nullptr || prop == nullptr) {
        return;
    }

    const auto& array_generic = *(sdk::TArray<void*>*)((uintptr_t)addr + prop->get_offset());

    if (array_generic.data == nullptr || array_generic.count == 0) {
        ImGui::Text("Empty array");
        return;
    }

    const auto inner = prop->get_inner();

    if (inner == nullptr) {
        ImGui::Text("Failed to get inner property");
        return;
    }
    
    const auto inner_c = inner->get_class();

    if (inner_c == nullptr) {
        ImGui::Text("Failed to get inner property class");
        return;
    }

    const auto inner_c_type = utility::narrow(inner_c->get_name().to_string());

    switch (utility::hash(inner_c_type)) {
    case "InterfaceProperty"_fnv:
    case "ObjectProperty"_fnv:
    {
        const auto& array_obj = *(sdk::TArray<sdk::UObject*>*)((uintptr_t)addr + prop->get_offset());
        auto parent_object = (sdk::UObject*)addr;

        if (!exists(parent_object)) {
            parent_object = nullptr;
        }

        for (auto obj : array_obj) {
            std::wstring name = obj->get_class()->get_fname().to_string() + L" " + obj->get_fname().to_string();
            const auto narrow_name = utility::narrow(name);

            if (ImGui::TreeNode(narrow_name.data())) {
                auto scope = m_path.enter(narrow_name);
                ui_handle_reachable_object(parent_object, obj, "ui object array");
                ImGui::TreePop();
            }
        }

        break;
    }
    case "StructProperty"_fnv:
    {
        // Not really an array of void* but we will skip over individual elements
        // using pointer arithmetic.
        const auto& array_obj = *(sdk::TArray<void*>*)((uintptr_t)addr + prop->get_offset());

        if (array_obj.data == nullptr || array_obj.count == 0) {
            ImGui::Text("Empty array");
            return;
        }

        const auto struct_prop = (sdk::FStructProperty*)inner;
        const auto strukt = struct_prop->get_struct();

        if (strukt == nullptr) {
            ImGui::Text("Cannot determine struct type");
            return;
        }
        
        auto element_size = strukt->get_struct_size();

        if (element_size == 0) {
            element_size = strukt->get_properties_size();

            if (element_size == 0) {
                ImGui::Text("Cannot determine struct size");
                return;
            }
        }

        for (size_t i = 0; i < array_obj.count; ++i) try {
            auto element = (void*)((uintptr_t)array_obj.data + (i * element_size));

            if (element != nullptr) {
                if (ImGui::TreeNode((void*)element, "Element %d", i)) {
                    // TODO: Figure out if this can be used with persistent states?
                    //auto scope = m_path.enter("Element " + std::to_string(i));
                    ui_handle_struct(element, strukt);
                    ImGui::TreePop();
                }
            }
        } catch(...) {
            ImGui::Text("Failed to display element %d", i);
        }

        break;
    }
    default:
        ImGui::Text("Array of %s (unsupported)", inner_c_type.data());
        break;
    };
}

void UObjectHook::ui_handle_struct(void* addr, sdk::UStruct* uclass) {
    if (uclass == nullptr) {
        return;
    }

    if (addr != nullptr && this->exists_unsafe((sdk::UObject*)addr) && uclass->is_a(sdk::UStruct::static_class())) {
        uclass = (sdk::UStruct*)addr;
        addr = nullptr;
    }

    // Display inheritance tree
    if (ImGui::TreeNode("Inheritance")) {
        for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
            if (ImGui::TreeNode(utility::narrow(super->get_full_name()).data())) {
                ui_handle_struct(addr, super);
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Functions")) {
        ui_handle_functions(addr, uclass);
        ImGui::TreePop();
    }

    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (ImGui::TreeNode("Properties")) {
        ui_handle_properties(addr, uclass);
        ImGui::TreePop();
    }
}

void* UObjectHook::add_object(void* rcx, void* rdx, void* r8, void* r9, void* stack1, void* stack2, void* stack3, void* stack4) {
    auto& hook = UObjectHook::get();
    auto result = hook->m_add_object_hook.unsafe_call<void*>(rcx, rdx, r8, r9, stack1, stack2, stack3, stack4);

    if (is_stalker2_uobjecthook_guard_enabled() &&
        !hook->m_stalker2_uobject_full_scan_requested.load(std::memory_order_relaxed)) {
        // Stalker2 can create huge UObject bursts during gameplay loads. In lazy
        // mode the common roots are tracked separately, so avoid the expensive
        // per-AddObject candidate validation hotpath entirely.
        hook->m_stalker2_lazy_stats.addobject_skips.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    {
        sdk::UObjectBase* obj = nullptr;

        if (use_dynamic_uobjecthook_candidate_guard()) {
            if (hook->m_add_object_guard_unreliable.load(std::memory_order_relaxed)) {
                return result;
            }

            struct Candidate {
                const char* name;
                sdk::UObjectBase* object;
            };

            static std::atomic<int> preferred_candidate{0}; // 0 unset, otherwise 1-based index into candidates.
            static std::atomic<int> logged_candidate{0};

            const Candidate candidates[] = {
                {"RCX", (sdk::UObjectBase*)rcx},
                {"RDX", (sdk::UObjectBase*)rdx},
                {"R8", (sdk::UObjectBase*)r8},
                {"R9", (sdk::UObjectBase*)r9},
                {"stack1", (sdk::UObjectBase*)stack1},
                {"stack2", (sdk::UObjectBase*)stack2},
                {"stack3", (sdk::UObjectBase*)stack3},
                {"stack4", (sdk::UObjectBase*)stack4},
            };

            constexpr size_t candidate_count = sizeof(candidates) / sizeof(candidates[0]);
            bool valid_candidates[candidate_count]{};

            // The original call has completed by this point, so a real AddObject candidate
            // should now be present in FUObjectArray. Requiring array membership keeps
            // wrong callsites from polluting UObjectHook with random UObject-looking values.
            constexpr bool require_array_member = true;

            for (size_t i = 0; i < candidate_count; ++i) {
                valid_candidates[i] = is_safe_uobject_candidate(*hook, candidates[i].object, require_array_member);
            }

            const auto previous = preferred_candidate.load(std::memory_order_relaxed);
            int selected_candidate{};

            if (previous > 0 && previous <= (int)candidate_count && valid_candidates[previous - 1]) {
                selected_candidate = previous;
            } else {
                for (size_t i = 0; i < candidate_count; ++i) {
                    if (valid_candidates[i]) {
                        selected_candidate = (int)i + 1;
                        break;
                    }
                }
            }

            if (selected_candidate != 0) {
                hook->m_add_object_guard_stats.valid_candidate_calls.fetch_add(1, std::memory_order_relaxed);
                preferred_candidate.store(selected_candidate, std::memory_order_relaxed);
                obj = candidates[selected_candidate - 1].object;

                const auto previous_logged = logged_candidate.exchange(selected_candidate, std::memory_order_relaxed);

                if (previous_logged != selected_candidate) {
                    SPDLOG_INFO("[UObjectHook] Guarded AddObject selected {} as UObjectBase*", candidates[selected_candidate - 1].name);
                }
            } else {
                constexpr uint64_t unreliable_threshold = 32;
                const auto no_candidate_count =
                    hook->m_add_object_guard_stats.no_candidate_calls.fetch_add(1, std::memory_order_relaxed) + 1;
                const auto valid_candidate_count =
                    hook->m_add_object_guard_stats.valid_candidate_calls.load(std::memory_order_relaxed);

                if (valid_candidate_count == 0 && no_candidate_count >= unreliable_threshold) {
                    const auto was_unreliable =
                        hook->m_add_object_guard_unreliable.exchange(true, std::memory_order_relaxed);

                    if (!was_unreliable) {
                        const auto object_array = sdk::FUObjectArray::get();
                        const auto object_count = object_array != nullptr ? object_array->get_object_count() : 0;
                        const bool stalker2_on_demand_tracking = should_use_stalker2_on_demand_uobject_tracking(object_count);

                        if (stalker2_on_demand_tracking) {
                            hook->m_uobject_array_scan_cursor = object_count;
                            hook->m_uobject_array_last_object_count = object_count;
                            hook->m_uobject_array_full_sweep_active = false;
                            hook->m_force_uobject_array_creation_scan.store(false, std::memory_order_relaxed);
                        } else {
                            hook->m_force_uobject_array_creation_scan.store(true, std::memory_order_relaxed);
                        }

                        hook->m_add_object_guard_stats.unreliable_transitions.fetch_add(1, std::memory_order_relaxed);

                        if (stalker2_on_demand_tracking) {
                            SPDLOG_WARN(
                                "[Stalker2][UObjectHook] Guarded AddObject produced {} calls with no safe UObject candidate; AddObject is unreliable, using on-demand roots instead of FUObjectArray backscan (rcx={:x}, rdx={:x}, r8={:x}, r9={:x})",
                                no_candidate_count,
                                (uintptr_t)rcx,
                                (uintptr_t)rdx,
                                (uintptr_t)r8,
                                (uintptr_t)r9);
                        } else {
                            SPDLOG_WARN(
                                "[UObjectHook] Guarded AddObject produced {} calls with no safe UObject candidate; treating AddObject hook as unreliable and relying on FUObjectArray scan (rcx={:x}, rdx={:x}, r8={:x}, r9={:x})",
                                no_candidate_count,
                                (uintptr_t)rcx,
                                (uintptr_t)rdx,
                                (uintptr_t)r8,
                                (uintptr_t)r9);
                        }
                    }
                } else {
                    SPDLOG_WARNING_EVERY_N_SEC(
                        2,
                        "[UObjectHook] Skipping AddObject call with no safe UObject candidate (rcx={:x}, rdx={:x}, r8={:x}, r9={:x})",
                        (uintptr_t)rcx,
                        (uintptr_t)rdx,
                        (uintptr_t)r8,
                        (uintptr_t)r9);
                }

                return result;
            }
        } else {
            static bool is_rcx = [&]() {
                if (!IsBadReadPtr(rcx, sizeof(void*)) && 
                    !IsBadReadPtr(*(void**)rcx, sizeof(void*)) &&
                    !IsBadReadPtr(**(void***)rcx, sizeof(void*))) 
                {
                    SPDLOG_INFO("[UObjectHook] RCX is UObjectBase*");
                    return true;
                } else {
                    SPDLOG_INFO("[UObjectHook] RDX is UObjectBase*");
                    return false;
                }
            }();

            if (is_rcx) {
                obj = (sdk::UObjectBase*)rcx;
            } else {
                obj = (sdk::UObjectBase*)rdx;
            }
        }

        ++hook->m_debug.constructor_calls;
        hook->add_new_object(obj);
    }

    return result;
}

void* UObjectHook::destructor(sdk::UObjectBase* object, void* rdx, void* r8, void* r9) {
    auto& hook = UObjectHook::get();

    {
        std::unique_lock _{hook->m_mutex};

        if (auto it = hook->m_meta_objects.find(object); it != hook->m_meta_objects.end()) {
            ++hook->m_debug.destructor_calls;

#ifdef VERBOSE_UOBJECTHOOK
            SPDLOG_INFO("Removing object {:x} {:s}", (uintptr_t)object, utility::narrow(it->second->full_name));
#endif
            if (should_incrementally_refresh_uobject_array() ||
                hook->m_force_uobject_array_creation_scan.load(std::memory_order_relaxed) ||
                hook->m_add_object_guard_unreliable.load(std::memory_order_relaxed))
            {
                if (const auto index_serial = get_uobject_index_serial(object); index_serial.has_value()) {
                    hook->m_destroyed_object_tombstones[object] = UObjectHook::DestroyedObjectTombstone{
                        index_serial->first,
                        index_serial->second,
                        std::chrono::steady_clock::now()
                    };
                }
            }

            hook->m_objects.erase(object);
            hook->m_motion_controller_attached_components.erase((sdk::USceneComponent*)object);
            hook->m_spawned_spheres.erase((sdk::USceneComponent*)object);
            hook->m_spawned_spheres_to_components.erase((sdk::USceneComponent*)object);
            hook->m_components_with_spheres.erase((sdk::USceneComponent*)object);

            if (object == hook->m_overlap_detection_actor) {
                hook->m_overlap_detection_actor = nullptr;
            }

            if (object == hook->m_overlap_detection_actor_left) {
                hook->m_overlap_detection_actor_left = nullptr;
            }

            if (object == hook->m_camera_attach.object) {
                hook->m_camera_attach.object = nullptr;
            }

            /*for (auto super = (sdk::UStruct*)it->second->uclass; super != nullptr;) {
                hook->m_objects_by_class[(sdk::UClass*)super].erase(object);

                // Just make sure we don't do any operations on super because it might be invalid...
                if (!hook->m_objects.contains(super)) {
                    SPDLOG_ERROR("Super for {:x} is not valid", (uintptr_t)object);
                    break;
                }

                super = super->get_super_struct();
            }*/

            for (auto super : it->second->super_classes) {
                hook->m_objects_by_class[super].erase(object);
            }

            hook->m_reusable_meta_objects.push_back(std::move(it->second));
            hook->m_meta_objects.erase(object);
        }
    }

    auto result = hook->m_destructor_hook.unsafe_call<void*>(object, rdx, r8, r9);

    return result;
}

void UObjectHook::PersistentProperties::save_to_file(std::optional<std::filesystem::path> path) try {
    if (!path.has_value()) {
        path = path_to_json;
    }

    if (!path.has_value()) {
        std::string concat_path{};
        for (const auto& p : this->path.path()) {
            concat_path += p;
        }

        const auto hash_str = std::to_string(utility::hash(concat_path)) + "_props.json";
        path = UObjectHook::get_persistent_dir() / hash_str;
    }

    std::filesystem::create_directories(path->parent_path());

    if (!std::filesystem::exists(path->parent_path())) {
        SPDLOG_ERROR("[UObjectHook] Failed to create directory {}", path->parent_path().string());
        return;
    }

    this->path_to_json = *path;

    nlohmann::json j = to_json();
    std::ofstream file(*path);
    file << j.dump(4);
} catch (const std::exception& e) {
    SPDLOG_ERROR("[UObjectHook] Failed to save persistent properties: {}", e.what());
} catch (...) {
    SPDLOG_ERROR("[UObjectHook] Failed to save persistent properties");
}

nlohmann::json UObjectHook::PersistentProperties::to_json() const {
    nlohmann::json json{};

    json["path"] = path.path();
    json["properties"] = nlohmann::json::array();
    json["type"] = "properties";
    json["hide"] = hide;
    json["hide_legacy"] = hide_legacy;

    for (const auto& prop : properties) {
        json["properties"].push_back({
            {"name", utility::narrow(prop->name)},
            {"data", prop->data.u64}
        });
    }

    return json;
}

std::shared_ptr<UObjectHook::PersistentProperties> UObjectHook::PersistentProperties::from_json(const nlohmann::json& json) try {
    if (!json.contains("path") || !json.contains("properties") || !json.contains("type")) {
        throw std::runtime_error("Missing path or properties");
    }

    // Make sure we're loading the right type
    if (!json["type"].is_string() || json["type"].get<std::string>() != "properties") {
        throw std::runtime_error("Wrong type");
    }

    auto result = std::make_shared<UObjectHook::PersistentProperties>();

    result->path = StatePath{json["path"].get<std::vector<std::string>>()};
    result->properties.clear();

    for (const auto& prop : json["properties"]) {
        if (!prop.contains("name") || !prop.contains("data")) {
            throw std::runtime_error("Missing name or data");
        }

        if (!prop["data"].is_number_unsigned()) {
            throw std::runtime_error("Data is not unsigned");
        }

        if (!prop["name"].is_string()) {
            throw std::runtime_error("Name is not string");
        }

        auto state = std::make_shared<PropertyState>();
        state->name = utility::widen(prop["name"].get<std::string>());
        state->data.u64 = prop["data"].get<uint64_t>();
        result->properties.push_back(state);
    }

    if (json.contains("hide") && json["hide"].is_boolean()) {
        result->hide = json["hide"].get<bool>();
    } else {
        result->hide = false;
    }

    if (json.contains("hide_legacy") && json["hide_legacy"].is_boolean()) {
        result->hide_legacy = json["hide_legacy"].get<bool>();
    } else {
        result->hide_legacy = false;
    }

    return result;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize persistent properties: {}", e.what());
    return nullptr;
} catch (...) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize persistent properties");
    return nullptr;
}

std::shared_ptr<UObjectHook::PersistentProperties> UObjectHook::PersistentProperties::from_json(std::filesystem::path json_path) {
    if (!std::filesystem::exists(json_path)) {
        return nullptr;
    }

    try {
        auto f = std::ifstream{json_path};

        if (f.is_open()) {
            const auto file_contents = std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};

            nlohmann::json data = nlohmann::json::parse(file_contents);

            return UObjectHook::PersistentProperties::from_json(data);
        }

        SPDLOG_ERROR("[UObjectHook] Failed to open JSON file {}", json_path.string());
        return nullptr;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}: {}", json_path.string(), e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}", json_path.string());
    }

    return nullptr;
}


std::vector<std::shared_ptr<UObjectHook::PersistentProperties>> UObjectHook::deserialize_all_persistent_properties() const try {
    const auto uobjecthook_dir = get_persistent_dir();

    if (!std::filesystem::exists(uobjecthook_dir)) {
        return {};
    }
    
    // Gather all .json files in this directory
    std::vector<std::filesystem::path> json_files{};
    for (const auto& p : std::filesystem::directory_iterator(uobjecthook_dir)) {
        if (p.path().extension() == ".json") {
            json_files.push_back(p.path());
        }
    }

    std::vector<std::shared_ptr<UObjectHook::PersistentProperties>> result{};
    for (const auto& json_file : json_files) {
        // load file
        auto state = UObjectHook::PersistentProperties::from_json(json_file);

        if (state != nullptr) {
            state->path_to_json = json_file;
            result.push_back(state);
            SPDLOG_INFO("[UObjectHook] Loaded persistent properties from {}", json_file.string());
        } else {
            SPDLOG_ERROR("[UObjectHook] {} does not appear to be a valid persistent properties file", json_file.string());
        }
    }

    return result;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize all persistent properties: {}", e.what());
    return {};
} catch (...) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize all persistent properties");
    return {};
}
