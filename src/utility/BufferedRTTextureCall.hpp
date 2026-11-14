#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <bddisasm.h>
#include <Windows.h>

namespace uevr::buffered_rt {
struct StackCell {
    uint32_t base{};
    int64_t displacement{};
    bool operator==(const StackCell&) const = default;
};

struct TextureCall {
    uintptr_t callsite{};
    uintptr_t receiver_cell{};
    uintptr_t command_list{};
    StackCell output{};
    uint32_t vtable_offset{};
    uint8_t length{};
    std::array<uint8_t, 15> bytes{};
    uintptr_t destroy{};
};

inline bool is_name_constructor(uintptr_t target, uintptr_t constructor) {
    return target != 0 && constructor != 0 && target == constructor;
}

inline std::optional<uintptr_t> add_displacement(uintptr_t base, int64_t displacement) {
    if (displacement >= 0) {
        const auto distance = static_cast<uintptr_t>(displacement);
        if (distance > (std::numeric_limits<uintptr_t>::max)() - base) { return {}; }
        return base + distance;
    }
    const auto distance = static_cast<uintptr_t>(-(displacement + 1)) + 1;
    if (distance > base) { return {}; }
    return base - distance;
}

inline bool reg(const ND_OPERAND& operand, uint32_t number) {
    return operand.Type == ND_OP_REG && operand.Size == 8 &&
        operand.Info.Register.Type == ND_REG_GPR && operand.Info.Register.Reg == number;
}

inline std::optional<StackCell> stack_cell(const ND_OPERAND& operand) {
    if (operand.Type != ND_OP_MEM) { return {}; }
    const auto& memory = operand.Info.Memory;
    if (!memory.HasBase || memory.HasIndex || memory.IsRipRel ||
        (memory.Base != NDR_RSP && memory.Base != NDR_RBP)) { return {}; }
    const auto displacement = memory.HasDisp ? static_cast<int64_t>(memory.Disp) : 0;
    if (displacement < -0x2000 || displacement > 0x2000) { return {}; }
    return StackCell{memory.Base, displacement};
}

// A name/setup helper is NOT a texture creator. Recover only the complete
// RHI(this, hidden-result, immediate-command-list, descriptor) call contract.
inline std::optional<TextureCall> find_after_name(std::span<const uint8_t> code, uintptr_t address) {
    constexpr size_t budget = 0x300;
    code = code.first((std::min)(code.size(), budget));
    if (!address || code.size() > (std::numeric_limits<uintptr_t>::max)() - address) { return {}; }
    struct Instruction { INSTRUX ix{}; size_t offset{}; };
    std::array<Instruction, 7> window{};
    size_t count{};
    size_t offset{};
    std::optional<TextureCall> result;
    while (offset < code.size()) {
        INSTRUX ix{};
        if (!ND_SUCCESS(NdDecodeEx(&ix, code.data() + offset, code.size() - offset,
                                  ND_CODE_64, ND_DATA_64)) || !ix.Length) { return result; }
        if (ix.Category == ND_CAT_RET) { break; }
        std::rotate(window.begin(), window.begin() + 1, window.end());
        window.back() = {ix, offset};
        ++count;
        offset += ix.Length;
        if (count < 7) { continue; }
        const auto& d = window[0].ix;
        const auto& r = window[1].ix;
        const auto& c = window[2].ix;
        const auto& o = window[3].ix;
        const auto& v = window[4].ix;
        const auto& call = window[5].ix;
        const auto& load = window[6].ix;
        // Require a stack descriptor immediately before the contiguous receiver,
        // command-list, hidden-result and vtable setup. No arbitrary next-call skip.
        if (d.Instruction != ND_INS_LEA || !reg(d.Operands[0], NDR_R9) || !stack_cell(d.Operands[1]) ||
            r.Instruction != ND_INS_MOV || !reg(r.Operands[0], NDR_RCX) ||
            c.Instruction != ND_INS_LEA || !reg(c.Operands[0], NDR_R8) ||
            o.Instruction != ND_INS_LEA || !reg(o.Operands[0], NDR_RDX) ||
            v.Instruction != ND_INS_MOV || !reg(v.Operands[0], NDR_RAX) ||
            call.Category != ND_CAT_CALL || load.Instruction != ND_INS_MOV ||
            !reg(load.Operands[0], NDR_RAX)) { continue; }
        if (r.Operands[1].Type != ND_OP_MEM || !r.Operands[1].Info.Memory.IsRipRel ||
            c.Operands[1].Type != ND_OP_MEM || !c.Operands[1].Info.Memory.IsRipRel ||
            v.Operands[1].Type != ND_OP_MEM || call.Operands[0].Type != ND_OP_MEM) { continue; }
        const auto& vm = v.Operands[1].Info.Memory;
        const auto& cm = call.Operands[0].Info.Memory;
        if (!vm.HasBase || vm.Base != NDR_RCX || vm.HasIndex || vm.IsRipRel ||
            (vm.HasDisp && vm.Disp != 0) || !cm.HasBase || cm.Base != NDR_RAX ||
            cm.HasIndex || cm.IsRipRel || !cm.HasDisp || cm.Disp < 0x40 ||
            cm.Disp >= 0x1000 || cm.Disp % sizeof(uintptr_t) != 0) { continue; }
        const auto output = stack_cell(o.Operands[1]);
        if (!output || stack_cell(load.Operands[1]) != output) { continue; }
        const auto receiver = add_displacement(address + window[1].offset + r.Length,
            static_cast<int64_t>(r.Operands[1].Info.Memory.Disp));
        const auto commands = add_displacement(address + window[2].offset + c.Length,
            static_cast<int64_t>(c.Operands[1].Info.Memory.Disp));
        if (!receiver || !commands || !*receiver || !*commands || result) { return {}; }
        TextureCall found{address + window[5].offset, *receiver, *commands, *output,
            static_cast<uint32_t>(cm.Disp), call.Length};
        std::memcpy(found.bytes.data(), code.data() + window[5].offset, call.Length);
        result = found;
    }
    return result;
}

// Require repeated engine-owned releases adjacent to this allocator. The
// decrement uses ESI; the live hook must separately verify ESI == UINT32_MAX.
inline std::optional<uintptr_t> find_destroy(std::span<const uint8_t> code, uintptr_t address) {
    constexpr std::array<uint8_t, 18> release{
        0x8b,0xc6,0xf0,0x0f,0xc1,0x41,0x08,0x25,0xff,0xff,0xff,0x3f,
        0x83,0xf8,0x01,0x75,0x05,0xe8};
    code = code.first((std::min)(code.size(), size_t{0x180}));
    if (!address || code.size() > (std::numeric_limits<uintptr_t>::max)() - address) { return {}; }
    size_t offset{}, matches{};
    std::optional<uintptr_t> result;
    while (offset < code.size()) {
        INSTRUX ix{};
        if (!ND_SUCCESS(NdDecodeEx(&ix, code.data() + offset, code.size() - offset,
                                  ND_CODE_64, ND_DATA_64)) || !ix.Length || ix.Category == ND_CAT_RET) { break; }
        if (code.size() - offset >= release.size() + 4 &&
            std::memcmp(code.data() + offset, release.data(), release.size()) == 0) {
            int32_t displacement{};
            std::memcpy(&displacement, code.data() + offset + release.size(), sizeof(displacement));
            const auto target = add_displacement(address + offset + release.size() + 4, displacement);
            if (!target || !*target || (result && result != target)) { return {}; }
            result = target;
            ++matches;
        }
        offset += ix.Length;
    }
    return matches >= 2 ? result : std::nullopt;
}

// FRHIResource::Destroy: mark the packed +8 counter for the engine's deferred
// deletion queue. Never substitute delete or the SDK's decrement-only release.
inline constexpr auto destroy_entry = std::to_array<uint8_t>({
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x0f,0x0d,0x49,0x08,
    0x8b,0x41,0x08,0x8b,0xd0,0x0f,0xba,0xea,0x1e,0xf0,0x0f,0xb1,0x51,
    0x08,0x75,0xf3,0xc1,0xe8,0x1e,0xa8,0x01});

struct ReleaseTexture {
    uintptr_t destroy{};
    void operator()(void* texture) const noexcept {
        if (!texture) { return; }
        // Matches the validated caller's packed counter, mask and last-ref test.
        const auto previous = static_cast<uint32_t>(InterlockedExchangeAdd(
            reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(texture) + 8), -1));
        if ((previous & 0x3fffffff) == 1) { reinterpret_cast<void(*)(void*)>(destroy)(texture); }
    }
};

// Operate on a local descriptor copy; never change the game's dimensions/format.
inline bool patch_descriptor(std::span<uint8_t> bytes, uint32_t scene_width, uint32_t scene_height,
                             uint32_t ui_width, uint32_t ui_height) {
    if (bytes.size() < 0x100 || !scene_width || !scene_height || !ui_width || !ui_height ||
        scene_width > 32768 || scene_height > 32768 || ui_width > 16384 || ui_height > 16384) { return false; }
    std::optional<size_t> extent;
    for (size_t i = 0; i < 0x60; i += sizeof(uint32_t)) {
        uint32_t width{}, height{};
        std::memcpy(&width, bytes.data() + i, sizeof(width));
        std::memcpy(&height, bytes.data() + i + 4, sizeof(height));
        if (width == scene_width && height == scene_height) {
            if (extent) { return false; }
            extent = i;
        }
    }
    if (!extent || bytes[*extent + 15] == 0 || bytes[*extent + 15] > 100) { return false; }
    std::memcpy(bytes.data() + *extent, &ui_width, sizeof(ui_width));
    std::memcpy(bytes.data() + *extent + 4, &ui_height, sizeof(ui_height));
    if (bytes[*extent + 15] == 18) { bytes[*extent + 15] = 2; }
    return true;
}

inline bool readable(uintptr_t address, size_t size) {
    if (address < 0x10000 || !size || size > 0x1000 ||
        size > (std::numeric_limits<uintptr_t>::max)() - address) { return false; }
    const auto end = address + size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return false; }
        const auto protection = memory.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY) { return false; }
        const auto base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > (std::numeric_limits<uintptr_t>::max)() - base ||
            base + memory.RegionSize <= address) { return false; }
        address = (std::min)(end, base + memory.RegionSize);
    }
    return true;
}

inline bool readable_texture(uintptr_t address) {
    return address % alignof(uintptr_t) == 0 && readable(address, sizeof(uintptr_t));
}

struct Arguments {
    uintptr_t receiver{}, output{}, command_list{}, descriptor{}, vtable{}, rsp{}, rbp{};
};

inline bool matches_arguments(const TextureCall& proof, const Arguments& args,
                              uintptr_t dynamic_rhi, uintptr_t current_receiver) {
    if (proof.output.base != NDR_RSP && proof.output.base != NDR_RBP) { return false; }
    const auto output = add_displacement(proof.output.base == NDR_RSP ? args.rsp : args.rbp,
                                         proof.output.displacement);
    const auto stack_end = add_displacement(args.rsp, 0x2000);
    return dynamic_rhi && args.receiver == dynamic_rhi && args.receiver == current_receiver &&
        args.command_list == proof.command_list && args.command_list != 0 &&
        output && args.output == *output && stack_end && args.output >= args.rsp &&
        args.output <= *stack_end - sizeof(uintptr_t) && args.descriptor >= args.rsp &&
        args.descriptor <= *stack_end - 0x100 &&
        (args.descriptor >= args.output + sizeof(uintptr_t) || args.descriptor + 0x100 <= args.output);
}
}
