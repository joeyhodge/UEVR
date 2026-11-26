#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uevr::ue426_renderer {

inline constexpr uint32_t minimum_size = 0x200;
inline constexpr uint32_t maximum_size = 0x4000;

struct FunctionRecord {
    uint32_t begin;
    uint32_t end;
    uint32_t unwind;

    bool operator==(const FunctionRecord&) const = default;
};
static_assert(sizeof(FunctionRecord) == 12);

struct Extent {
    uint32_t begin;
    uint32_t end;

    bool operator==(const Extent&) const = default;
};

template <typename Lookup, typename Read>
std::optional<Extent> extend_short_renderer(
    uint32_t callback_return, size_t image_size, Lookup&& lookup, Read&& read)
{
    using UnwindHeader = std::array<uint8_t, 4>;
    const auto header = [&](const FunctionRecord& entry) -> std::optional<UnwindHeader> {
        UnwindHeader bytes{};
        if (entry.begin == 0 || entry.end <= entry.begin || entry.end > image_size ||
            entry.unwind == 0 || (entry.unwind & 3) != 0 || entry.unwind >= image_size ||
            bytes.size() > image_size - entry.unwind || !read(entry.unwind, bytes.data(), bytes.size()) ||
            bytes[1] > entry.end - entry.begin)
        {
            return {};
        }
        return bytes;
    };
    const auto parent = [&](const FunctionRecord& entry, const UnwindHeader& info) -> std::optional<FunctionRecord> {
        constexpr uint8_t version_one_chaininfo = 1 | (4 << 3);
        const auto offset = info.size() + ((size_t{info[2]} + 1) & ~size_t{1}) * sizeof(uint16_t);
        FunctionRecord owner{};
        if (info[0] != version_one_chaininfo || offset + sizeof(owner) > image_size - entry.unwind ||
            !read(entry.unwind + offset, &owner, sizeof(owner)))
        {
            return {};
        }
        return owner;
    };

    if (callback_return >= image_size) {
        return {};
    }
    const auto callback = lookup(callback_return);
    const auto callback_header = callback ? header(*callback) : std::nullopt;
    if (!callback_header || callback_return < callback->begin || callback_return >= callback->end ||
        callback_return - callback->begin < 3)
    {
        return {};
    }

    const auto owner = parent(*callback, *callback_header);
    if (!owner || owner->begin >= owner->end || owner->end != callback->begin ||
        callback->end - owner->begin >= minimum_size)
    {
        return {};
    }
    const auto root = lookup(owner->begin);
    const auto root_header = root && *root == *owner ? header(*root) : std::nullopt;
    if (!root_header || (*root_header)[0] != 1 || (*root_header)[1] == 0 ||
        (*root_header)[3] != (*callback_header)[3])
    {
        return {};
    }

    // MSVC can put the rest of the renderer in a contiguous sibling of the
    // callback's unwind range. Count it only if both link to the same root.
    const auto next = lookup(callback->end);
    const auto next_header = next ? header(*next) : std::nullopt;
    if (!next_header || next->begin != callback->end ||
        next->end - root->begin < minimum_size || next->end - root->begin > maximum_size ||
        (*next_header)[3] != (*root_header)[3] || parent(*next, *next_header) != owner)
    {
        return {};
    }
    return Extent{root->begin, next->end};
}

} // namespace uevr::ue426_renderer
