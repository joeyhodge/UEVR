#pragma once

#include "UE58NativeFix.hpp"
#include "UE59NativeContracts.hpp"

namespace uevr::ue59_native {

// The family prefix/span ABI is unchanged. FSceneInterface's added virtuals
// move this call from 0x4d8 to 0x510; do not relax the UE5.8 contract.
inline constexpr std::array<uint8_t,24> renderer_frame_code{
    0x49,0x8b,0x07,0x44,0x38,0xa0,0xb7,0,0,0,0x74,0x0c,
    0x48,0x8b,0x06,0x48,0x8b,0xce,0xff,0x90,0x10,0x05,0,0
};
inline bool renderer_layout(const sdk::discovery::Memory& memory, uintptr_t renderer) {
    using namespace ue58_native::contract;
    const bool shipping = sdk::discovery::relative_address(renderer,0x1b4) &&
        ue58_native::matches(memory,renderer+0x2a,renderer_span_code,renderer_span_mask) &&
        ue58_native::matches(memory,renderer+0x19c,renderer_frame_code,renderer_frame_mask);
    return shipping || (sdk::discovery::relative_address(renderer,0x488) &&
        ue58_native::matches(memory,renderer+0x30,contract::development_renderer_code,contract::development_renderer_mask));
}

inline constexpr const char* development_copy_signature =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 48 8D 71 08 45 33 FF 4C 89 3E";

template<class FunctionSize>
std::optional<ue58_native::FamilyFunctions> family_functions(const sdk::discovery::Memory& memory,
    uintptr_t copy, uintptr_t base, size_t image_size, FunctionSize&& function_size) {
    if (const auto stock=ue58_native::family_functions(memory,copy,base,image_size,function_size)) { return stock; }
    using namespace contract;
    const auto contains=[&](uintptr_t p,size_t n) { return base>=0x10000 && p>=base && p-base<image_size && n<=image_size-(p-base); };
    if (!contains(copy,development_family_copy_code.size()) || function_size(copy)!=development_family_copy_code.size() ||
        !ue58_native::matches(memory,copy,development_family_copy_code,development_family_copy_mask)) { return {}; }
    int32_t displacement{};
    if (!memory.load(copy+0x25,displacement)) { return {}; }
    const auto table=sdk::discovery::relative_address(copy+0x29,displacement);
    uintptr_t deleting{};
    if (!table || !contains(*table,sizeof(deleting)) || !memory.load(*table,deleting) ||
        !contains(deleting,development_family_delete_code.size()) || function_size(deleting)!=development_family_delete_code.size() ||
        !ue58_native::matches(memory,deleting,development_family_delete_code,development_family_delete_mask) ||
        !memory.load(deleting+0x10,displacement)) { return {}; }
    const auto destroy=sdk::discovery::relative_address(deleting+0x14,displacement);
    if (!destroy || !contains(*destroy,development_family_destroy_code.size()) || function_size(*destroy)!=development_family_destroy_code.size() ||
        !ue58_native::matches(memory,*destroy,development_family_destroy_code,development_family_destroy_mask) ||
        !memory.load(*destroy+9,displacement) || sdk::discovery::relative_address(*destroy+0xd,displacement)!=table) { return {}; }
    return ue58_native::FamilyFunctions{copy,*destroy,*table,true};
}

class FamilyClone {
public:
    bool initialize(const sdk::discovery::Memory& memory,uintptr_t source,uintptr_t vtable,
        const ue58_native::FamilyFunctions& functions,const char*& reason) {
        if (m_stock || m_wide) { reason="family transaction was already initialized"; return false; }
        if (functions.development_layout) {
            return m_wide.emplace().initialize(memory,source,vtable,functions,reason);
        }
        return m_stock.emplace().initialize(memory,source,vtable,functions,reason);
    }
    uintptr_t address() { return m_stock ? m_stock->address() : m_wide ? m_wide->address() : 0; }
    bool original_render_safe() const { return m_stock ? m_stock->original_render_safe() : !m_wide || m_wide->original_render_safe(); }
    bool finish(const sdk::discovery::Memory& memory,uintptr_t source) {
        return m_stock ? m_stock->finish(memory,source) : m_wide && m_wide->finish(memory,source);
    }
private:
    std::optional<ue58_native::FamilyClone> m_stock;
    std::optional<ue58_native::FamilyCloneStorage<0x1b8,0x178>> m_wide;
};
} // namespace uevr::ue59_native
