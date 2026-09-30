#pragma once

#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace utility::uobject::candidate {
struct Layout {
    size_t size{};
    size_t class_offset{};
    size_t name_offset{};
};

constexpr bool supported(const Layout& layout) {
    return layout.size >= sizeof(uintptr_t) && layout.size <= 64 &&
        layout.class_offset <= layout.size - sizeof(uintptr_t) &&
        layout.name_offset <= layout.size - sizeof(uint64_t);
}

template <typename Copier>
bool read_checked(uintptr_t address, void* output, size_t size, Copier&& copy) {
    if (!output || address < 0x10000 || !size || size > 64 ||
        address > (std::numeric_limits<uintptr_t>::max)() - size) { return false; }
    std::array<uint8_t, 64> bytes{};
    SIZE_T copied{};
    if (!copy(address, bytes.data(), size, copied) || copied != size) { return false; }
    std::memcpy(output, bytes.data(), size);
    return true;
}

inline bool read(uintptr_t address, void* output, size_t size) {
    // Validate just the requested bytes, not the extent of a large allocation.
    // No cached page permissions, raw-read fallback, or partial publication.
    return read_checked(address, output, size,
        [](uintptr_t source, void* destination, size_t count, SIZE_T& copied) {
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(source),
                destination, count, &copied) != 0;
        });
}

struct Header {
    uintptr_t vtable{}, uclass{};
    uint32_t name_index{};
};

template <typename Reader>
std::optional<Header> header(uintptr_t address, const Layout& layout, Reader&& reader) {
    std::array<uint8_t, 64> bytes{};
    if (!supported(layout) || !reader(address, bytes.data(), layout.size)) { return {}; }
    Header result;
    std::memcpy(&result.vtable, bytes.data(), sizeof(result.vtable));
    std::memcpy(&result.uclass, bytes.data() + layout.class_offset, sizeof(result.uclass));
    std::memcpy(&result.name_index, bytes.data() + layout.name_offset, sizeof(result.name_index));
    return result;
}

template <typename Reader>
std::optional<uintptr_t> class_pointer(uintptr_t address, const Layout& layout, Reader&& reader) {
    const auto object = header(address, layout, reader);
    if (!object || !object->uclass || (object->name_index >> 16) >= 0x4000) { return {}; }
    const auto klass = header(object->uclass, layout, reader);
    uintptr_t unused{};
    if (!klass || (klass->name_index >> 16) >= 0x4000 ||
        !reader(object->vtable, &unused, sizeof(unused)) ||
        !reader(klass->vtable, &unused, sizeof(unused)) ||
        !reader(klass->uclass, &unused, sizeof(unused))) { return {}; }
    return object->uclass;
}
}
