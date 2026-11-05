#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include "ProSpiNativeCode.hpp"

namespace uevr::prospi::players {
struct Layout {
    uintptr_t base{};
    uint32_t image_size{}, enabled{}, bounds{}, transform{}, handoff{};
    bool r15{};
};
using CodeProof = native_code::Proof;
// Narrow, independently checked native-player contracts. Unknown updates keep
// the existing signature-based path; they never borrow the other edition's fields.
template<class Read> std::optional<Layout> verified_layout(Read read, uintptr_t base, uint32_t image_size, uint32_t timestamp) {
    if (image_size == 0x2e02a000 && timestamp == 1790730967u) { // ebaseball
        constexpr CodeProof proofs[]{
            {0x58513b0, "488db7e0070000f30f110ef30f1187f4070000f30f1187ec070000"},
            {0x58513cd, "488db7e0070000488bd6488b8f88030000e8fd76f9ff"},
            {0x585159a, "80bfe006000000740984db7405"},
            {0x5851c90, "e86bcdf8ff4c8b08450fb6c4488b5708488bc841ff91e8000000f30f108f70070000488bcfe8c6e4"},
            {0x57e8ae0, "48895c2408574883ec20488bd9488bfa488d0dfa990a15e8547bb2014885ff74124889bba0010000488b5c24304883c4205fc3488b83880100004885c0740f488b40304883c014488983a0010000488b5c24304883c4205fc3"},
            {0x580e660, "48895c2408574883ec20488bf9488bda488d0df5480815e8d41fb0010f1003488bc70f11070f104b100f114f100f1043200f1147200f104b30488b5c24300f114f304883c4205fc3"},
        };
        for (const auto proof : proofs) { if (!native_code::matches(read, base, image_size, proof)) { return {}; } }
        return Layout{base, image_size, 0x6e0, 0x7e0, 0x388, 0x5851c98, false};
    }
    if (image_size == 0x2beeb000 && timestamp == 1773341438u) { // 2025
        constexpr CodeProof proofs[]{
            {0x5600053, "488d9790090000488b8f80030000e88a65faff"},
            {0x56001da, "488d9790090000488b8f80030000e80364faff"},
            {0x56001ee, "80bfd806000000740984db7405"},
            {0x5600801, "e8aac9f9ff4c8b08450fb6c7488b5708488bc841ff91e0000000f30f108f60070000488bcfe8f5e6"},
            {0x55a65f0, "48895c2408574883ec20488bd9488bfa488d0d8c1e8f13e8242ddc014885ff74124889bba0010000488b5c24304883c4205fc3488b83880100004885c0740f488b40304883c014488983a0010000488b5c24304883c4205fc3"},
            {0x55cb420, "48895c2408574883ec20488bf9488bda488d0df9d88c13e8f4ded9010f1003488bc70f11070f104b100f114f100f1043200f1147200f104b30488b5c24300f114f304883c4205fc3"},
        };
        for (const auto proof : proofs) { if (!native_code::matches(read, base, image_size, proof)) { return {}; } }
        return Layout{base, image_size, 0x6d8, 0x990, 0x380, 0x5600809, true};
    }
    return {};
}
}
