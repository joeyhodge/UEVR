#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <sdk/BoundedDiscovery.hpp>

#include "UE58OwnedUITexture.hpp"

namespace uevr::pbz_ui {

// Verified in PBZ's resource destructor, dimension getters and secondary vtable.
inline constexpr uintptr_t render_target_offset = 0x50;
inline constexpr uintptr_t render_target_texture_offset = 0x58;
inline constexpr uintptr_t owner_offset = 0x90;
inline constexpr uintptr_t width_offset = 0xbc;
inline constexpr uintptr_t height_offset = 0xc0;
inline constexpr uintptr_t resource_size = 0xd0;
inline constexpr auto size_x_accessor = std::to_array<uint8_t>({0x8b, 0x81, 0xbc, 0, 0, 0, 0xc3});
inline constexpr auto size_y_accessor = std::to_array<uint8_t>({0x8b, 0x81, 0xc0, 0, 0, 0, 0xc3});

// Read only the non-linked resource path. Never execute the fallback vcall.
inline constexpr auto native_resource_prefix = std::to_array<uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xd0, 0, 0, 0,
    0x33, 0xdb, 0x48, 0x85, 0xc0, 0x74, 0x09, 0x48, 0x8b, 0x58, 0x20,
    0x48, 0x85, 0xdb});

// Historical fixture; runtime proof below follows argument origins before hooking.
inline constexpr uintptr_t slate_inputs_offset = 0x36;
inline constexpr auto slate_inputs_contract = std::to_array<uint8_t>({
    0x49, 0x8b, 0x59, 0x18, 0x4c, 0x8b, 0xf1, 0x49, 0x8b, 0x41, 0x08,
    0x49, 0x8b, 0xf9, 0x48, 0x89, 0x55, 0x28, 0x4d, 0x8b, 0xe8});

inline bool slate_inputs_proven(const sdk::discovery::Memory& memory, uintptr_t function,
    uintptr_t image, size_t image_size) {
    constexpr size_t max_bytes = 0x180, max_instructions = 64;
    const auto contains = [&](uintptr_t address, size_t count) {
        return image >= 0x10000 && image_size <= UINTPTR_MAX - image && address >= image &&
            address - image <= image_size && count <= image_size - (address - image);
    };
    if (!contains(function, max_bytes) || !memory.executable ||
        !memory.executable(memory.context, function, 1)) { return false; }

    enum class Origin : uint8_t { unknown, renderer, output, graph, inputs, stack, command_list, zero };
    struct Value { Origin origin{}; int64_t offset{}; };
    struct Slot { int64_t offset{}; size_t size{}; Value value{}; bool used{}; };
    std::array<Value, 16> registers{};
    registers[1].origin = Origin::renderer;
    registers[2].origin = Origin::output;
    registers[4].origin = Origin::stack;
    registers[8].origin = Origin::graph;
    registers[9].origin = Origin::inputs;
    std::array<Slot, 32> slots{};
    const auto gpr = [](const ND_OPERAND& op) {
        return op.Type == ND_OP_REG && op.Info.Register.Type == ND_REG_GPR &&
            op.Info.Register.Reg < 16 && !op.Info.Register.IsHigh8;
    };
    const auto plain_memory = [](const ND_OPERAND& op) {
        return op.Type == ND_OP_MEM && op.Info.Memory.Seg != 4 && op.Info.Memory.Seg != 5;
    };
    const auto stack_address = [&](const ND_OPERAND& op) -> std::optional<int64_t> {
        if (!plain_memory(op) || !op.Info.Memory.HasBase || op.Info.Memory.HasIndex ||
            op.Info.Memory.IsRipRel || op.Info.Memory.Base >= registers.size() ||
            op.Info.Memory.BaseSize != 8 || registers[op.Info.Memory.Base].origin != Origin::stack) { return {}; }
        const auto displacement = static_cast<int64_t>(op.Info.Memory.Disp);
        if (displacement < -0x4000 || displacement > 0x1000) { return {}; }
        const auto offset = registers[op.Info.Memory.Base].offset + displacement;
        return offset >= -0x4000 && offset <= 0x1000 ? std::optional{offset} : std::nullopt;
    };
    const auto store = [&](int64_t offset, size_t size, Value value) {
        if (!size || size > 16 || offset < -0x4000 || offset > 0x1000) { return false; }
        for (auto& slot : slots) {
            if (slot.used && offset < slot.offset + int64_t(slot.size) && slot.offset < offset + int64_t(size)) { slot.used = false; }
        }
        if (value.origin == Origin::unknown) { return true; }
        for (auto& slot : slots) {
            if (!slot.used) { slot = {offset, size, value, true}; return true; }
        }
        return false;
    };
    const auto retained = [&](Origin origin) {
        for (size_t reg : {size_t{3}, size_t{5}, size_t{6}, size_t{7}, size_t{12}, size_t{13}, size_t{14}, size_t{15}}) {
            if (registers[reg].origin == origin) { return true; }
        }
        for (const auto& slot : slots) {
            // The first callee owns [rsp, rsp+0x20), even if the caller stored a pointer there.
            if (slot.used && slot.size == 8 && slot.value.origin == origin &&
                registers[4].origin == Origin::stack && slot.offset >= registers[4].offset + 0x20) { return true; }
        }
        return false;
    };

    bool viewport{}, elements{}, command_list{};
    auto ip = function;
    for (size_t step = 0; step < max_instructions && ip - function < max_bytes; ++step) {
        const auto instruction = sdk::discovery::decode(memory, ip);
        if (!instruction || instruction->decoded.Length > max_bytes - (ip - function)) { return false; }
        const auto& ix = instruction->decoded;
        const auto& a = ix.Operands[0];
        const auto& b = ix.Operands[1];
        if (instruction->bytes[0] == 0xe8 && ix.Length == 5) {
            int32_t displacement{};
            std::memcpy(&displacement, instruction->bytes.data() + 1, 4);
            const auto target = sdk::discovery::relative_address(ip + ix.Length, displacement);
            return viewport && elements && command_list && retained(Origin::renderer) &&
                retained(Origin::output) && retained(Origin::graph) && retained(Origin::inputs) &&
                registers[1].origin == Origin::command_list && registers[2].origin == Origin::zero &&
                target && contains(*target, 1) && memory.executable(memory.context, *target, 1);
        }
        if (sdk::discovery::padding(*instruction)) {
            // Padding is allowed, but never a branch or a thunk to another body.
        } else if (ix.Instruction == ND_INS_PUSH && ix.ExpOperandsCount == 1 && gpr(a) && a.Size == 8 && a.Info.Register.Reg != 4) {
            if (registers[4].origin != Origin::stack || registers[4].offset < -0x3ff8) { return false; }
            registers[4].offset -= 8;
            if (!store(registers[4].offset, 8, registers[a.Info.Register.Reg])) { return false; }
        } else if (ix.Instruction == ND_INS_SUB && ix.ExpOperandsCount == 2 && gpr(a) && a.Size == 8 &&
            a.Info.Register.Reg == 4 && b.Type == ND_OP_IMM && b.Info.Immediate.Imm > 0 && b.Info.Immediate.Imm <= 0x1000) {
            if (registers[4].origin != Origin::stack || registers[4].offset < -0x3000) { return false; }
            registers[4].offset -= int64_t(b.Info.Immediate.Imm);
        } else if (ix.Instruction == ND_INS_LEA && ix.ExpOperandsCount == 2 && gpr(a) && a.Size == 8) {
            const auto offset = stack_address(b);
            if (!offset) { return false; }
            registers[a.Info.Register.Reg] = {Origin::stack, *offset};
        } else if (ix.Instruction == ND_INS_XOR && ix.ExpOperandsCount == 2 && gpr(a) && gpr(b)) {
            if (a.Info.Register.Reg == 4) { return false; }
            registers[a.Info.Register.Reg] = a.Info.Register.Reg == b.Info.Register.Reg && a.Size >= 4
                ? Value{Origin::zero} : Value{};
        } else if (ix.Instruction == ND_INS_MOV && ix.ExpOperandsCount == 2 && gpr(a)) {
            Value value{};
            if (a.Size != 8) { return false; }
            if (gpr(b) && b.Size == 8) {
                value = registers[b.Info.Register.Reg];
            } else if (plain_memory(b) && b.Size == 8) {
                if (const auto offset = stack_address(b)) {
                    for (const auto& slot : slots) {
                        if (slot.used && slot.offset == *offset && slot.size == 8) { value = slot.value; break; }
                    }
                } else if (b.Info.Memory.IsRipRel) {
                    const auto target = sdk::discovery::relative_address(ip + ix.Length, static_cast<int64_t>(b.Info.Memory.Disp));
                    if (!target || !contains(*target, 8)) { return false; }
                } else if (b.Info.Memory.HasBase && !b.Info.Memory.HasIndex && b.Info.Memory.Base < registers.size() &&
                    b.Info.Memory.BaseSize == 8) {
                    const auto origin = registers[b.Info.Memory.Base].origin;
                    const auto displacement = b.Info.Memory.Disp;
                    if (origin == Origin::inputs && (displacement == 8 || displacement == 0x18)) {
                        elements |= displacement == 8; viewport |= displacement == 0x18;
                    } else if (origin == Origin::graph && displacement == 0xc0) {
                        command_list = true;
                        value.origin = Origin::command_list;
                    } else { return false; }
                } else { return false; }
            } else { return false; }
            if (a.Info.Register.Reg == 4 && value.origin != Origin::stack) { return false; }
            registers[a.Info.Register.Reg] = value;
        } else if ((ix.Instruction == ND_INS_MOV || ix.Instruction == ND_INS_MOVAPS) && ix.ExpOperandsCount == 2 && a.Type == ND_OP_MEM) {
            const auto offset = stack_address(a);
            if (!offset || (ix.Instruction == ND_INS_MOVAPS && b.Type != ND_OP_REG)) { return false; }
            const auto value = gpr(b) && b.Size == 8 && a.Size == 8 ? registers[b.Info.Register.Reg] : Value{};
            if (!store(*offset, a.Size, value)) { return false; }
        } else { return false; }
        ip += ix.Length;
    }
    return false;
}

constexpr bool valid_extent(uint32_t width, uint32_t height) {
    return width >= 320 && height >= 240 && width <= 16384 && height <= 16384 &&
        uint64_t{width} * height <= 64ull * 1024 * 1024;
}

struct Creation {
    bool exact_game{}, dx12{}, inputs_proven{}, game_data{}, engine{}, stable_slate{}, main_target{};
    uint32_t width{}, height{};
};

constexpr bool can_request(const Creation& state) {
    // Menus may not construct any scene family. Do not wait for that callback.
    return state.exact_game && state.dx12 && state.inputs_proven && state.game_data &&
        state.engine && state.stable_slate && state.main_target && valid_extent(state.width, state.height);
}

template <typename Read, typename Validate>
std::optional<ue58_owned_ui::Resource> find_owned_resource(
    uintptr_t owner, size_t owner_size, uint32_t width, uint32_t height,
    const Read& read, const Validate& validate) {
    constexpr auto limit = (std::numeric_limits<uintptr_t>::max)();
    if (owner < 0x10000 || owner_size < 2 * sizeof(uintptr_t) ||
        owner_size > ue58_owned_ui::max_owner_size || owner > limit - owner_size ||
        !valid_extent(width, height))
    {
        return std::nullopt;
    }

    std::optional<ue58_owned_ui::Resource> result;
    for (uintptr_t offset = sizeof(uintptr_t); offset + 2 * sizeof(uintptr_t) <= owner_size; offset += sizeof(uintptr_t)) {
        uintptr_t resource{}, render_resource{}, actual_owner{}, rhi{}, target_rhi{};
        uint32_t actual_width{}, actual_height{};
        if (!read(owner + offset, resource) || !read(owner + offset + sizeof(uintptr_t), render_resource)) {
            return std::nullopt;
        }
        if (resource < 0x10000 || resource != render_resource || (resource & 7) != 0 ||
            resource > limit - resource_size)
        {
            continue;
        }
        if (!read(resource + owner_offset, actual_owner) || actual_owner != owner ||
            !read(resource + ue58_owned_ui::texture_rhi_offset, rhi) || rhi < 0x10000 || (rhi & 7) != 0 ||
            !read(resource + render_target_texture_offset, target_rhi) || target_rhi != rhi ||
            !read(resource + width_offset, actual_width) || actual_width != width ||
            !read(resource + height_offset, actual_height) || actual_height != height ||
            !validate(resource, rhi))
        {
            continue;
        }
        if (result) {
            return std::nullopt;
        }
        result = ue58_owned_ui::Resource{offset, resource, rhi, owner_size};
    }
    return result;
}

template <typename Read>
bool resource_matches(uintptr_t owner, const ue58_owned_ui::Resource& expected,
    uint32_t width, uint32_t height, const Read& read) {
    constexpr auto limit = (std::numeric_limits<uintptr_t>::max)();
    if (owner < 0x10000 || expected.owner_size < 2 * sizeof(uintptr_t) ||
        expected.owner_size > ue58_owned_ui::max_owner_size || owner > limit - expected.owner_size ||
        expected.private_resource_offset < sizeof(uintptr_t) || (expected.private_resource_offset & 7) != 0 ||
        expected.private_resource_offset > expected.owner_size - 2 * sizeof(uintptr_t) ||
        expected.resource < 0x10000 || expected.resource > limit - resource_size ||
        expected.rhi_texture < 0x10000 || !valid_extent(width, height))
    {
        return false;
    }
    uintptr_t resource{}, rt_resource{}, actual_owner{}, rhi{}, target_rhi{};
    uint32_t actual_width{}, actual_height{};
    return read(owner + expected.private_resource_offset, resource) && resource == expected.resource &&
        read(owner + expected.private_resource_offset + sizeof(uintptr_t), rt_resource) && rt_resource == resource &&
        read(resource + owner_offset, actual_owner) && actual_owner == owner &&
        read(resource + ue58_owned_ui::texture_rhi_offset, rhi) && rhi == expected.rhi_texture &&
        read(resource + render_target_texture_offset, target_rhi) && target_rhi == rhi &&
        read(resource + width_offset, actual_width) && actual_width == width &&
        read(resource + height_offset, actual_height) && actual_height == height;
}

} // namespace uevr::pbz_ui
