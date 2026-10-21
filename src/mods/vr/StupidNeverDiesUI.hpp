#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <d3d12.h>
#include "utility/BufferedRTTextureCall.hpp"

namespace uevr::stupid_never_dies_ui {

inline bool target(std::wstring_view path, bool dx12) {
    if (!dx12) { return false; }
    path = path.substr(path.find_last_of(L"/\\") + 1);
    constexpr std::wstring_view name = L"stupidneverdies.exe";
    if (path.size() != name.size()) { return false; }
    for (size_t i = 0; i < path.size(); ++i) {
        const auto ch = path[i] >= L'A' && path[i] <= L'Z' ? path[i] + (L'a' - L'A') : path[i];
        if (ch != name[i]) { return false; }
    }
    return true;
}

// The declared UE5.4.4 game has a backported void DrawWindow(renderer, graph,
// inputs) ABI, not the stock legacy viewport ABI or UE5.5 hidden-output ABI.
inline constexpr size_t entry_offset = 0x29;
inline constexpr auto entry_contract = std::to_array<uint8_t>({
    0x49,0x8b,0x70,0x18,0x4c,0x8b,0xea,0x0f,0x29,0x70,0xa8,0x33,0xd2,
    0x0f,0x29,0x78,0x98,0x49,0x8b,0xf8,0x49,0x8b,0x40,0x08,
    0x49,0x8b,0x8d,0xf0,0x00,0x00,0x00});
inline constexpr size_t command_list_offset = 0xf0;

struct Inputs {
    uintptr_t renderer{}, elements{}, window{}, viewport_info{};
    uintptr_t post_process_requests{};
    int32_t request_count{};
    uint32_t padding{};
    int32_t cursor_x{}, cursor_y{}, min_x{}, min_y{}, max_x{}, max_y{};
    float scale{};
};
static_assert(sizeof(Inputs) == 0x50 && offsetof(Inputs, min_x) == 0x38 && offsetof(Inputs, scale) == 0x48);

template<class Read, class IsObject>
std::optional<Inputs> inputs(uintptr_t renderer, uintptr_t address, Read&& read, IsObject&& is_object) {
    Inputs in{};
    uintptr_t head{};
    if (!renderer || !address || !read(address, &in, sizeof(in)) || in.renderer != renderer ||
        !in.elements || !read(in.elements, &head, sizeof(head)) ||
        !is_object(renderer) || !is_object(in.window) || !is_object(in.viewport_info) ||
        in.request_count < 0 || in.request_count > 256 ||
        (in.request_count != 0 && !in.post_process_requests) ||
        !std::isfinite(in.scale) || in.scale < -1 || in.scale > 8) { return {}; }
    const auto width = int64_t{in.max_x} - in.min_x;
    const auto height = int64_t{in.max_y} - in.min_y;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) { return {}; }
    return in;
}

struct Call {
    uintptr_t callsite{}, name{}, create{}, registration{};
    buffered_rt::StackCell output{};
    uintptr_t lookup{}, lookup_function{};
};

// Prove both CreateRenderTarget(hidden-result, raw-texture, name) and the
// immediately following GraphBuilder.RegisterExternalTexture(ref, flags).
template<class Read>
std::optional<Call> find(std::span<const uint8_t> code, uintptr_t address, Read&& read) {
    using namespace buffered_rt;
    code = code.first((std::min)(code.size(), size_t{0x1800}));
    if (!address || code.size() > (std::numeric_limits<uintptr_t>::max)() - address) { return {}; }
    struct Instruction { INSTRUX ix{}; size_t offset{}; };
    std::array<Instruction, 14> window{};
    size_t offset{}, count{};
    std::optional<Call> result;
    while (offset < code.size()) {
        INSTRUX ix{};
        if (!ND_SUCCESS(NdDecodeEx(&ix, code.data() + offset, code.size() - offset,
                                  ND_CODE_64, ND_DATA_64)) || !ix.Length) { break; }
        if (ix.Category == ND_CAT_RET) { break; }
        std::rotate(window.begin(), window.begin() + 1, window.end());
        window.back() = {ix, offset};
        ++count; offset += ix.Length;
        if (count < window.size()) { continue; }
        const auto& lookup_input = window[0].ix;
        const auto& preserved_output = window[1].ix;
        const auto& lookup = window[2].ix;
        const auto& saved_result = window[3].ix;
        const auto& test = window[4].ix;
        const auto& branch = window[5].ix;
        const auto& name = window[6].ix;
        const auto& texture = window[7].ix;
        const auto& output = window[8].ix;
        const auto& create = window[9].ix;
        const auto& returned = window[10].ix;
        const auto& flags = window[11].ix;
        const auto& builder = window[12].ix;
        const auto& registration = window[13].ix;
        const auto r8d = [](const ND_OPERAND& op) {
            return op.Type == ND_OP_REG && op.Size == 4 && op.Info.Register.Type == ND_REG_GPR && op.Info.Register.Reg == NDR_R8;
        };
        if (lookup_input.Instruction != ND_INS_MOV || !reg(lookup_input.Operands[0], NDR_RDX) || !reg(lookup_input.Operands[1], NDR_RBX) ||
            preserved_output.Instruction != ND_INS_MOV || !stack_cell(preserved_output.Operands[0]) || !reg(preserved_output.Operands[1], NDR_RBX) ||
            lookup.Length != 5 || code[window[2].offset] != 0xe8 ||
            saved_result.Instruction != ND_INS_MOV || !stack_cell(saved_result.Operands[0]) || !reg(saved_result.Operands[1], NDR_RAX) ||
            test.Instruction != ND_INS_TEST || !reg(test.Operands[0], NDR_RAX) || !reg(test.Operands[1], NDR_RAX) ||
            branch.Length != 2 || code[window[5].offset] != 0x75 ||
            name.Instruction != ND_INS_LEA || !reg(name.Operands[0], NDR_R8) ||
            name.Operands[1].Type != ND_OP_MEM || !name.Operands[1].Info.Memory.IsRipRel ||
            texture.Instruction != ND_INS_MOV || !reg(texture.Operands[0], NDR_RDX) || !reg(texture.Operands[1], NDR_RBX) ||
            output.Instruction != ND_INS_LEA || !reg(output.Operands[0], NDR_RCX) || !stack_cell(output.Operands[1]) ||
            create.Length != 5 || code[window[9].offset] != 0xe8 ||
            returned.Instruction != ND_INS_MOV || !reg(returned.Operands[0], NDR_RDX) || !reg(returned.Operands[1], NDR_RAX) ||
            flags.Instruction != ND_INS_XOR || !r8d(flags.Operands[0]) || !r8d(flags.Operands[1]) ||
            builder.Instruction != ND_INS_MOV || !reg(builder.Operands[0], NDR_RCX) || !reg(builder.Operands[1], NDR_R13) ||
            registration.Length != 5 || code[window[13].offset] != 0xe8) { continue; }
        const auto join = add_displacement(address + window[5].offset + 2, static_cast<int8_t>(code[window[5].offset + 1]));
        if (!join || *join <= address + window[13].offset + 5 || *join > address + window[13].offset + 0x80) { continue; }
        const auto name_address = add_displacement(address + window[6].offset + name.Length,
            static_cast<int64_t>(name.Operands[1].Info.Memory.Disp));
        constexpr std::u16string_view expected = u"SlateOutputTexture";
        std::array<char16_t, expected.size() + 1> text{};
        if (!name_address || !read(*name_address, text.data(), sizeof(text)) ||
            std::u16string_view{text.data(), expected.size()} != expected || text.back() != 0) { continue; }
        int32_t first{}, second{}, query{};
        std::memcpy(&first, code.data() + window[9].offset + 1, 4);
        std::memcpy(&second, code.data() + window[13].offset + 1, 4);
        std::memcpy(&query, code.data() + window[2].offset + 1, 4);
        const auto create_address = add_displacement(address + window[9].offset + 5, first);
        const auto register_address = add_displacement(address + window[13].offset + 5, second);
        const auto query_address = add_displacement(address + window[2].offset + 5, query);
        if (!create_address || !*create_address || !register_address || !*register_address ||
            !query_address || !*query_address || *query_address == *create_address || *query_address == *register_address ||
            *create_address == *register_address || result) { return {}; }
        result = Call{address + window[9].offset, *name_address, *create_address, *register_address,
            *stack_cell(output.Operands[1]), address + window[2].offset, *query_address};
    }
    return result;
}

inline bool output_matches(const Call& call, uintptr_t rcx, uintptr_t rsp, uintptr_t rbp) {
    if (call.output.base != NDR_RSP && call.output.base != NDR_RBP) { return false; }
    const auto expected = buffered_rt::add_displacement(call.output.base == NDR_RSP ? rsp : rbp, call.output.displacement);
    const auto end = buffered_rt::add_displacement(rsp, 0x2000);
    return expected && end && rcx == *expected && rcx >= rsp && rcx <= *end - 8 && rcx % 8 == 0;
}

inline bool color_target(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width > 0 && desc.Width <= 32768 &&
        desc.Height > 0 && desc.Height <= 32768 && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0;
}

inline bool ui_target(const D3D12_RESOURCE_DESC& desc, uint32_t width, uint32_t height) {
    // Unreal's PF_B8G8R8A8 normally allocates a TYPELESS resource with typed
    // RTV/SRV views. UEVR's existing UI consumer uses BGRA views and copies.
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return color_target(desc) && width != 0 && height != 0 && desc.Width == width && desc.Height == height &&
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 && bgra;
}
}
