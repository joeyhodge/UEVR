#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace uevr::hifi {
inline constexpr char16_t hbk_ue427_branch[] = u"++ue+ue_main+4.27hbk";

inline std::optional<size_t> find_hbk_ue427_branch(std::span<const uint8_t> data) {
    const auto* first = reinterpret_cast<const uint8_t*>(hbk_ue427_branch);
    const auto* last = first + sizeof(hbk_ue427_branch);
    auto cursor = data.begin();
    while (cursor != data.end()) {
        const auto found = std::search(cursor, data.end(), first, last);
        if (found == data.end()) {
            return std::nullopt;
        }
        const auto offset = static_cast<size_t>(found - data.begin());
        if (offset % sizeof(char16_t) == 0) {
            return offset;
        }
        cursor = found + 1;
    }
    return std::nullopt;
}

inline bool matches_game(std::wstring_view path, bool ue427) {
    if (!ue427) {
        return false;
    }
    path = path.substr(path.find_last_of(L"/\\") + 1);
    constexpr std::wstring_view expected = L"hi-fi-rush.exe";
    if (path.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < path.size(); ++i) {
        auto c = path[i];
        if (c >= L'A' && c <= L'Z') {
            c += L'a' - L'A';
        }
        if (c != expected[i]) {
            return false;
        }
    }
    return true;
}

// This is the retained original stub, not a replacement syscall or a guessed syscall number.
inline bool matches_original_stub(std::span<const uint8_t> candidate, uintptr_t candidate_address,
    std::span<const uint8_t> original, uintptr_t entry_address) {
    if (candidate.size() < 13 || original.size() < 8 || original[0] != 0x4c || original[1] != 0x8b ||
        original[2] != 0xd1 || original[3] != 0xb8 || candidate[8] != 0xe9 ||
        std::memcmp(candidate.data(), original.data(), 8) != 0) {
        return false;
    }
    int32_t displacement{};
    std::memcpy(&displacement, candidate.data() + 9, sizeof(displacement));
    return candidate_address + 13 + static_cast<int64_t>(displacement) == entry_address + 8;
}
}
