#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <numeric>
#include <vector>

#include "utility/UObjectCandidateSnapshot.hpp"
#include "utility/UObjectMetadataFilter.hpp"

int test_uobject_browser_cost() {
    int failures{};
    const auto expect = [&](bool result, const char* message) {
        if (!result) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    std::vector<size_t> values(1024);
    std::iota(values.begin(), values.end(), 0);
    size_t lookups{};
    const auto names = [&](size_t value) { ++lookups; return L"Property_" + std::to_wstring(1024 - value); };
    const auto rows = utility::uobject::named_browser_values(values, names);
    expect(rows.size() == values.size() && lookups == values.size(), "one engine name conversion per property, never per comparison");
    expect(std::is_sorted(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.name < b.name; }),
        "owned labels preserve lexicographic browser ordering");
    expect(lookups == values.size(), "sorting and displaying saved labels do not reenter the name resolver");
    const auto next = utility::uobject::named_browser_values(values, [](size_t) { return L"Renamed"; });
    expect(next.front().name == L"Renamed" && next.back().value == values.back(),
        "fresh labels each draw and stable equal-name ordering; no cross-frame pointer cache");
    const auto empty = utility::uobject::named_browser_values(std::vector<size_t>{}, names);
    expect(empty.empty() && lookups == values.size(), "an empty browser does no name work");

    namespace candidate = utility::uobject::candidate;
    std::array<uint8_t, 64> output;
    output.fill(0xA5);
    const auto unchanged = output;
    size_t copies{};
    const auto short_copy = [&](uintptr_t, void* target, size_t size, SIZE_T& copied) {
        ++copies;
        std::memset(target, 0, size);
        copied = size - 1;
        return true;
    };
    expect(!candidate::read_checked(0x10000, output.data(), 24, short_copy) && output == unchanged,
        "short successful copies never publish partial headers");
    const auto failed_copy = [&](uintptr_t, void* target, size_t size, SIZE_T& copied) {
        ++copies;
        std::memset(target, 0, size);
        copied = size;
        return false;
    };
    expect(!candidate::read_checked(0x10000, output.data(), 24, failed_copy) && output == unchanged,
        "failed copies never publish even if a byte count was reported");
    expect(!candidate::read_checked(0, output.data(), 24, short_copy) &&
        !candidate::read_checked(UINTPTR_MAX - 4, output.data(), 24, short_copy) &&
        !candidate::read_checked(0x10000, nullptr, 24, short_copy) &&
        !candidate::read_checked(0x10000, output.data(), 0, short_copy) &&
        !candidate::read_checked(0x10000, output.data(), 65, short_copy) && copies == 2,
        "invalid read bounds never reach Windows");

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const size_t page = info.dwPageSize;
    auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    expect(pages != nullptr, "candidate fixture allocates");
    if (!pages) { return failures; }
    const auto base = reinterpret_cast<uintptr_t>(pages);
    const auto write = [&](size_t offset, auto value) { std::memcpy(pages + offset, &value, sizeof(value)); };
    for (const auto layout : {candidate::Layout{0x28, 0x10, 0x18}, candidate::Layout{0x30, 0x18, 0x20},
                             candidate::Layout{0x30, 0x10, 0x18}}) {
        std::memset(pages, 0, page * 2);
        write(0x100, base + 0x300);
        write(0x180, base + 0x300);
        write(0x100 + layout.class_offset, base + 0x180);
        write(0x180 + layout.class_offset, base + 0x180);
        write(0x100 + layout.name_offset, uint32_t{15});
        write(0x180 + layout.name_offset, uint32_t{16});
        size_t reads{};
        const auto reader = [&](uintptr_t address, void* dest, size_t size) {
            ++reads;
            return candidate::read(address, dest, size);
        };
        const auto klass = candidate::class_pointer(base + 0x100, layout, reader);
        expect(klass == base + 0x180 && reads == 5, "stock, case-preserving, and KH3 headers use five bounded reads");
        write(0x100 + layout.name_offset, uint32_t{0x40000000});
        expect(!candidate::class_pointer(base + 0x100, layout, reader), "implausible object names fail closed");
        write(0x100 + layout.name_offset, uint32_t{15});
        write(0x180 + layout.name_offset, uint32_t{0x40000000});
        expect(!candidate::class_pointer(base + 0x100, layout, reader), "implausible class names fail closed");
        write(0x180 + layout.name_offset, uint32_t{16});
        write(0x100, uintptr_t{});
        expect(!candidate::class_pointer(base + 0x100, layout, reader), "null vtables fail closed");
        write(0x100, base + 0x300);
        write(0x180 + layout.class_offset, uintptr_t{1});
        expect(!candidate::class_pointer(base + 0x100, layout, reader), "invalid class-of-class pointers fail closed");
    }
    expect(!candidate::supported({65, 16, 24}) && !candidate::supported({24, 24, 16}) &&
        !candidate::supported({24, 16, 24}), "unsupported header shapes stay on the existing fallback");
    DWORD previous{};
    expect(VirtualProtect(pages, page, PAGE_READONLY, &previous) != 0, "fixture becomes readonly");
    expect(candidate::read(base, output.data(), 64), "readonly data remains readable");
    expect(VirtualProtect(pages, page, PAGE_READWRITE | PAGE_GUARD, &previous) != 0, "fixture gains guard");
    output = unchanged;
    expect(!candidate::read(base, output.data(), 24) && !candidate::read(base, output.data(), 24) && output == unchanged,
        "guard pages reject repeatedly without publishing bytes");
    MEMORY_BASIC_INFORMATION region{};
    VirtualQuery(pages, &region, sizeof(region));
    expect((region.Protect & PAGE_GUARD) != 0, "reader does not consume the guard");
    expect(VirtualProtect(pages, page, PAGE_NOACCESS, &previous) != 0, "fixture becomes inaccessible");
    expect(!candidate::read(base, output.data(), 24), "protection is not cached between reads");
    expect(VirtualProtect(pages, page, PAGE_READWRITE, &previous) != 0, "first page restored");
    expect(VirtualProtect(pages + page, page, PAGE_NOACCESS, &previous) != 0, "second page inaccessible");
    expect(!candidate::read(base + page - 8, output.data(), 24) && output == unchanged,
        "cross-page partial copy fails closed");
    expect(VirtualProtect(pages + page, page, PAGE_READWRITE | PAGE_GUARD, &previous) != 0, "second page guarded");
    expect(!candidate::read(base + page - 8, output.data(), 24), "cross-page guard fails closed");
    VirtualQuery(pages + page, &region, sizeof(region));
    expect((region.Protect & PAGE_GUARD) != 0, "cross-page read preserves guard");
    expect(VirtualProtect(pages + page, page, PAGE_READONLY, &previous) != 0, "second page readonly");
    expect(candidate::read(base + page - 8, output.data(), 24), "fully readable cross-region spans remain accepted");
    expect(VirtualFree(pages, page, MEM_DECOMMIT) != 0, "first page decommitted");
    expect(!candidate::read(base, output.data(), 24), "decommitted memory fails closed");
    VirtualFree(pages, 0, MEM_RELEASE);
    return failures;
}
