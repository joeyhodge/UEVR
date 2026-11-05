#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace uevr::prospi::native_code {
struct Proof { uint32_t rva; std::string_view hex; };
template<class Read> bool matches(Read& read, uintptr_t base, uint32_t size, Proof proof) {
    std::array<uint8_t, 128> actual{};
    const auto n = proof.hex.size()/2;
    if (base < 0x10000 || base >= 0x0000800000000000ull || size > 0x0000800000000000ull-base ||
        proof.hex.size()%2 || !n || n > actual.size() || proof.rva >= size || n > size-proof.rva ||
        !read(base+proof.rva, actual.data(), n)) { return false; }
    const auto digit = [](char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1; };
    for (size_t i{}; i < n; ++i) {
        const auto high = digit(proof.hex[i*2]), low = digit(proof.hex[i*2+1]);
        if (high < 0 || low < 0 || actual[i] != ((high << 4) | low)) { return false; }
    }
    return true;
}
}
