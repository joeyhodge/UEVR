#pragma once

#include <optional>
#include <string>

#include "SatisfactoryModular.hpp"

namespace sdk { struct IConsoleVariable; class FSceneViewFamily; }

namespace uevr::satisfactory {

bool is_current_runtime();
std::optional<Image> renderer_image();
bool console_abi_ready();
sdk::IConsoleVariable* find_console_variable(const std::wstring& name, bool floating);

class NativeFamilyClone {
public:
    NativeFamilyClone() = default;
    NativeFamilyClone(const NativeFamilyClone&) = delete;
    NativeFamilyClone& operator=(const NativeFamilyClone&) = delete;
    ~NativeFamilyClone();
    bool initialize(sdk::FSceneViewFamily* source, uintptr_t expected_vtable, const char*& reason);
    bool finish(sdk::FSceneViewFamily* source);
    bool original_render_safe() const { return m_original_render_safe; }
    sdk::FSceneViewFamily* get();
private:
    alignas(16) std::array<uint8_t, family_size> m_storage{};
    std::array<uintptr_t, 4> m_borrowed{};
    std::optional<FamilyFunctions> m_functions{};
    bool m_constructed{};
    bool m_original_render_safe{true};
};

} // namespace uevr::satisfactory
