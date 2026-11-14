#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <sdk/BoundedDiscovery.hpp>

namespace utility::accessor {
inline bool pointer(uintptr_t value) {
    return value >= 0x10000 && value < 0x0000800000000000ull && (value & 7) == 0;
}

namespace detail {
struct PointerReader {
    enum class Kind { Unknown, Scalar, Pointer, Stack };
    struct Value {
        uintptr_t bits{};
        Kind kind{};
        uint8_t depth{};
    };
    const sdk::discovery::Memory& memory;
    uintptr_t function;
    std::array<uint8_t, 256> code_bytes{};
    size_t code_size{};
    std::array<Value, 16> registers{};
    std::array<Value, 64> stack{};
    std::optional<bool> zero;
    size_t offset{}, branches{}, loads{};
    static std::optional<size_t> reg(const ND_OPERAND& op) {
        if (op.Type != ND_OP_REG || op.Info.Register.Type != ND_REG_GPR || op.Info.Register.Reg >= 16 || (op.Size != 4 && op.Size != 8)) {
            return {};
        }
        return op.Info.Register.Reg;
    }
    static std::optional<size_t> slot(uintptr_t address) {
        if (address < 0xf00 || address >= 0x1100 || (address & 7)) {
            return {};
        }
        return (address - 0xf00) / 8;
    }
    std::optional<Value> address(const ND_OPERAND& op) const {
        if (op.Type != ND_OP_MEM || !op.Info.Memory.HasBase || op.Info.Memory.HasIndex || op.Info.Memory.IsRipRel ||
            op.Info.Memory.Base >= 16) {
            return {};
        }
        const auto base = registers[op.Info.Memory.Base];
        if (base.kind != Kind::Stack && base.kind != Kind::Pointer) {
            return {};
        }
        const auto displacement = static_cast<int64_t>(op.Info.Memory.Disp);
        if (displacement < -256 || displacement > 0x1000 || (base.kind == Kind::Pointer && displacement < 0)) {
            return {};
        }
        const auto result = sdk::discovery::relative_address(base.bits, displacement);
        if (!result) {
            return {};
        }
        return Value{*result, base.kind, base.depth};
    }
    std::optional<Value> operand(const ND_OPERAND& op) {
        if (const auto index = reg(op)) {
            auto value = registers[*index];
            if (op.Size == 4) {
                if (value.kind != Kind::Scalar) {
                    return {};
                }
                value.bits = static_cast<uint32_t>(value.bits);
            }
            return value;
        }
        if (op.Type == ND_OP_IMM) {
            return Value{op.Info.Immediate.Imm, Kind::Scalar, 0};
        }
        const auto source = address(op);
        if (!source || op.Size != 8) {
            return {};
        }
        if (source->kind == Kind::Stack) {
            const auto index = slot(source->bits);
            return index ? std::optional{stack[*index]} : std::nullopt;
        }
        if (!pointer(source->bits) || source->depth >= 3 || ++loads > 8) {
            return {};
        }
        uintptr_t value{};
        if (!memory.load(source->bits, value) || (value && !pointer(value))) {
            return {};
        }
        return Value{value, Kind::Pointer, static_cast<uint8_t>(source->depth + 1)};
    }
    sdk::discovery::Memory code_memory() {
        return {this,
            [](void* context, uintptr_t p, void* out, size_t size) {
                const auto& b = *static_cast<PointerReader*>(context);
                if (p < b.function || p - b.function > b.code_size || size > b.code_size - (p - b.function)) {
                    return false;
                }
                std::memcpy(out, b.code_bytes.data() + (p - b.function), size);
                return true;
            },
            [](void* context, uintptr_t p, size_t size) {
                const auto& b = *static_cast<PointerReader*>(context);
                return p >= b.function && p - b.function <= b.code_size && size <= b.code_size - (p - b.function);
            }};
    }
    std::optional<uintptr_t> run(uintptr_t object) {
        const auto capture = [&](size_t size) {
            return memory.executable && memory.read && memory.executable(memory.context, function, size) &&
                   memory.read(memory.context, function, code_bytes.data(), size);
        };
        if (capture(code_bytes.size())) {
            code_size = code_bytes.size();
        } else {
            // Page/function boundaries are uncommon; find a readable executable
            // prefix in at most eight probes, rather than querying memory per opcode.
            size_t low{}, high = code_bytes.size();
            while (low + 1 < high) {
                const auto mid = low + (high - low) / 2;
                if (capture(mid)) {
                    low = mid;
                } else {
                    high = mid;
                }
            }
            if (!low || !capture(low)) {
                return {};
            }
            code_size = low;
        }
        registers[NDR_RCX] = {object, Kind::Pointer, 0};
        registers[NDR_RSP] = {0x1000, Kind::Stack, 0};
        const auto code = code_memory();
        for (size_t instructions = 0; instructions < 64 && offset < 256; ++instructions) {
            const auto decoded = sdk::discovery::decode(code, function + offset);
            if (!decoded || !decoded->decoded.Length || decoded->decoded.HasSeg) {
                return {};
            }
            const auto& ix = decoded->decoded;
            const auto next = offset + ix.Length;
            if (sdk::discovery::padding(*decoded)) {
                offset = next;
                continue;
            }
            if (ix.Category == ND_CAT_RET) {
                if (ix.Instruction != ND_INS_RETN || (ix.HasImm1 && ix.Immediate1 != 0) || registers[NDR_RSP].kind != Kind::Stack ||
                    registers[NDR_RSP].bits != 0x1000) {
                    return {};
                }
                const auto result = registers[NDR_RAX];
                return result.kind == Kind::Pointer && result.depth && pointer(result.bits) ? std::optional{result.bits} : std::nullopt;
            }
            const auto destination = ix.OperandsCount ? reg(ix.Operands[0]) : std::nullopt;
            if (ix.Instruction == ND_INS_PUSH || ix.Instruction == ND_INS_POP) {
                auto& sp = registers[NDR_RSP];
                if (!destination || ix.Operands[0].Size != 8 || *destination == NDR_RSP || sp.kind != Kind::Stack) {
                    return {};
                }
                if (ix.Instruction == ND_INS_PUSH) {
                    const auto index = slot(sp.bits - 8);
                    if (!index) {
                        return {};
                    }
                    sp.bits -= 8;
                    stack[*index] = registers[*destination];
                } else {
                    const auto index = slot(sp.bits);
                    if (!index) {
                        return {};
                    }
                    registers[*destination] = stack[*index];
                    sp.bits += 8;
                }
            } else if (ix.Instruction == ND_INS_MOV && ix.OperandsCount >= 2) {
                const auto value = operand(ix.Operands[1]);
                if (!value) {
                    return {};
                }
                if (destination && ix.Operands[0].Size == 8) {
                    registers[*destination] = *value;
                } else if (destination && ix.Operands[0].Size == 4 && value->kind == Kind::Scalar) {
                    registers[*destination] = {static_cast<uint32_t>(value->bits), Kind::Scalar, 0};
                } else {
                    const auto target = address(ix.Operands[0]);
                    const auto index = target && target->kind == Kind::Stack ? slot(target->bits) : std::nullopt;
                    if (!index || ix.Operands[0].Size != 8) {
                        return {};
                    } // Never model an engine-object write.
                    stack[*index] = *value;
                }
            } else if (ix.Instruction == ND_INS_LEA && destination && ix.OperandsCount >= 2) {
                const auto value = address(ix.Operands[1]);
                if (!value || value->kind != Kind::Stack || !slot(value->bits) || ix.Operands[0].Size != 8) {
                    return {};
                }
                registers[*destination] = *value;
            } else if (ix.Instruction == ND_INS_XOR && destination && ix.OperandsCount >= 2 && reg(ix.Operands[1]) == destination &&
                       ix.Operands[0].Size == ix.Operands[1].Size) {
                registers[*destination] = {0, Kind::Scalar, 0};
                zero = true;
            } else if ((ix.Instruction == ND_INS_ADD || ix.Instruction == ND_INS_SUB) && destination == NDR_RSP && ix.OperandsCount >= 2 &&
                       ix.Operands[0].Size == 8 && ix.Operands[1].Type == ND_OP_IMM) {
                auto& sp = registers[NDR_RSP];
                const auto amount = ix.Operands[1].Info.Immediate.Imm;
                if (sp.kind != Kind::Stack || amount > 256) {
                    return {};
                }
                const auto result = sdk::discovery::relative_address(
                    sp.bits, ix.Instruction == ND_INS_SUB ? -static_cast<int64_t>(amount) : static_cast<int64_t>(amount));
                if (!result || !slot(*result)) {
                    return {};
                }
                sp.bits = *result;
                zero.reset();
            } else if ((ix.Instruction == ND_INS_TEST || ix.Instruction == ND_INS_CMP) && ix.OperandsCount >= 2) {
                const auto left = operand(ix.Operands[0]), right = operand(ix.Operands[1]);
                if (!left || !right || left->kind == Kind::Unknown || right->kind == Kind::Unknown) {
                    return {};
                }
                zero = ix.Instruction == ND_INS_TEST ? (left->bits & right->bits) == 0 : left->bits == right->bits;
            } else if (ix.Category == ND_CAT_COND_BR || ix.Category == ND_CAT_UNCOND_BR) {
                const auto& bytes = decoded->bytes;
                const bool equal = bytes[0] == 0x74 || (bytes[0] == 0x0f && bytes[1] == 0x84);
                const bool unequal = bytes[0] == 0x75 || (bytes[0] == 0x0f && bytes[1] == 0x85);
                const bool jump = bytes[0] == 0xeb || bytes[0] == 0xe9;
                if ((!equal && !unequal && !jump) || (!jump && !zero) || ++branches > 8 || ix.Operands[0].Type != ND_OP_OFFS) {
                    return {};
                }
                const auto target = sdk::discovery::relative_address(next, ix.Operands[0].Info.RelativeOffset.Rel);
                if (!target || *target < next || *target >= 256) {
                    return {};
                }
                if (jump || (equal ? *zero : !*zero)) {
                    // Do not let an interior byte masquerade as a new instruction.
                    for (size_t boundary = next; boundary < *target;) {
                        const auto skipped = sdk::discovery::decode(code, function + boundary);
                        if (!skipped || !skipped->decoded.Length || boundary + skipped->decoded.Length > *target) {
                            return {};
                        }
                        boundary += skipped->decoded.Length;
                    }
                    offset = *target;
                    continue;
                }
            } else {
                return {};
            }
            offset = next;
        }
        return {};
    }
};
} // namespace detail

// Interpret a small getter, never execute it. Only member reads, register moves,
// null branches and a private model of its stack are permitted. A linked-resource
// fallback that calls another virtual is deliberately unsupported.
inline std::optional<uintptr_t> read_pointer(const sdk::discovery::Memory& memory, uintptr_t function, uintptr_t object) {
    if (!pointer(object) || function < 0x10000 || function >= 0x0000800000000000ull - 256) {
        return {};
    }
    detail::PointerReader reader{memory, function};
    return reader.run(object);
}
} // namespace utility::accessor
