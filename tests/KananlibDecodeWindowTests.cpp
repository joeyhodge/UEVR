#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

// The separate negative-control build includes the actual unpatched header.
#ifdef UEVR_KANANLIB_TEST_ORIGINAL_SCAN_HEADER
#include UEVR_KANANLIB_TEST_ORIGINAL_SCAN_HEADER
#else
#include <utility/Emulation.hpp>
#include <utility/Scan.hpp>
#ifndef UEVR_KANANLIB_READABLE_WINDOW_BACKPORT
#error The regression suite must use the production Kananlib decode-window overlay
#endif
#endif

namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

struct Mapping {
    std::size_t page{};
    uint8_t* data{};

    Mapping() {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page = info.dwPageSize;
        data = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 3,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (data) {
            std::memset(data, 0xCC, page * 3);
            DWORD old{};
            if (!VirtualProtect(data + page * 2, page, PAGE_NOACCESS, &old)) {
                VirtualFree(data, 0, MEM_RELEASE);
                data = nullptr;
            }
        }
    }
    ~Mapping() {
        if (data) {
            VirtualFree(data, 0, MEM_RELEASE);
        }
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
};

struct Observation {
    uintptr_t address{};
    uint8_t length{};
    uintptr_t target{};
};

std::vector<Observation> decode(uint8_t* code, std::size_t limit = 16) {
    std::vector<Observation> seen;
    utility::exhaustive_decode(code, limit, [&](utility::ExhaustionContext& ctx) {
        seen.push_back({ctx.addr, ctx.instrux.Length, ctx.resolved_target});
        return utility::ExhaustionResult::CONTINUE;
    });
    return seen;
}

template<std::size_t N>
bool matches(const std::vector<Observation>& seen, uint8_t* code,
             const std::array<uint8_t, N>& lengths) {
    if (seen.size() != N) {
        return false;
    }
    auto address = reinterpret_cast<uintptr_t>(code);
    for (std::size_t i = 0; i < N; ++i) {
        if (seen[i].address != address || seen[i].length != lengths[i]) {
            return false;
        }
        address += lengths[i];
    }
    return true;
}

void test_page_boundary(Mapping& memory) {
    // Upstream's four-instruction fixture must work at every distance from the boundary.
    constexpr std::array<uint8_t, 9> code{0xB8, 0xFF, 0, 0, 0, 0x04, 0x01, 0x9F, 0xC3};
    for (std::size_t gap = 0; gap <= 80; ++gap) {
        auto* start = memory.data + memory.page * 2 - code.size() - gap;
        std::memcpy(start, code.data(), code.size());
        expect(matches(decode(start), start, std::array<uint8_t, 4>{5, 2, 1, 1}),
            "all complete instructions decode before a no-access page");
    }

    auto* end = memory.data + memory.page * 2;
    end[-1] = 0xC3;
    expect(matches(decode(end - 1), end - 1, std::array<uint8_t, 1>{1}),
        "a one-byte return at the boundary decodes");
    for (std::size_t partial = 1; partial < 5; ++partial) {
        auto* start = end - partial - 1;
        start[0] = 0x90;
        start[1] = 0xB8;
        if (partial > 1) {
            std::memset(start + 2, 0, partial - 1);
        }
        expect(matches(decode(start), start, std::array<uint8_t, 1>{1}),
            "a truncated instruction is rejected without losing the preceding nop");
    }
}

void test_readable_and_unreadable_pages(Mapping& memory) {
    constexpr std::array<uint8_t, 6> code{0xB8, 0x12, 0x34, 0x56, 0x78, 0xC3};
    auto* start = memory.data + memory.page - 2;
    std::memcpy(start, code.data(), code.size());
    expect(matches(decode(start), start, std::array<uint8_t, 2>{5, 1}),
        "a complete instruction spanning two readable pages is unchanged");
    expect(decode(memory.data + memory.page * 2).empty(), "unreadable starts fail closed");
    expect(decode(nullptr).empty(), "null starts fail closed");

#ifndef UEVR_KANANLIB_TEST_ORIGINAL_SCAN_HEADER
    using utility::uevr_kananlib_backport::readable_decode_window;
    expect(readable_decode_window(memory.data + 128) == 64, "interior decode window remains 64 bytes");
    expect(readable_decode_window(start) == 64, "readable page crossings retain the full window");
    expect(readable_decode_window(memory.data + memory.page * 2 - 1) == 1,
        "the window stops exactly before an unreadable page");
    expect(readable_decode_window(memory.data + memory.page * 2) == 0,
        "the helper rejects unreadable starts");
#endif
}

void test_existing_traversal(Mapping& memory) {
    auto* start = memory.data + 128;
    constexpr std::array<uint8_t, 10> code{0x75, 0x02, 0x90, 0xC3, 0xB8, 1, 0, 0, 0, 0xC3};
    std::memcpy(start, code.data(), code.size());
    const auto seen = decode(start);
    expect(seen.size() == 5 && seen[0].address == reinterpret_cast<uintptr_t>(start) &&
        seen[1].address == reinterpret_cast<uintptr_t>(start + 2) &&
        seen[2].address == reinterpret_cast<uintptr_t>(start + 3) &&
        seen[3].address == reinterpret_cast<uintptr_t>(start + 4) &&
        seen[4].address == reinterpret_cast<uintptr_t>(start + 9),
        "conditional branch traversal order stays unchanged");
    expect(!seen.empty() && seen[0].target == reinterpret_cast<uintptr_t>(start + 4),
        "conditional branch targets stay unchanged");

    int count{};
    utility::exhaustive_decode(start, 16, [&](INSTRUX&, uintptr_t) {
        ++count;
        return utility::ExhaustionResult::STEP_OVER;
    });
    expect(count == 3, "legacy callback and STEP_OVER semantics stay unchanged");
    count = 0;
    utility::exhaustive_decode(start, 16, [&](utility::ExhaustionContext&) {
        ++count;
        return utility::ExhaustionResult::BREAK;
    });
    expect(count == 1, "BREAK semantics stay unchanged");
    expect(decode(start, 0).empty(), "a zero instruction budget stays empty");
    expect(decode(start, 1).size() == 2, "the existing per-branch instruction budget stays unchanged");

    constexpr std::array<uint8_t, 2> loop{0xEB, 0xFE};
    std::memcpy(start, loop.data(), loop.size());
    expect(matches(decode(start), start, std::array<uint8_t, 1>{2}),
        "seen-address cycle rejection stays unchanged");

    std::array<uint8_t, 8> lea{0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};
    const int32_t displacement = -39;
    std::memcpy(lea.data() + 3, &displacement, sizeof(displacement));
    std::memcpy(start, lea.data(), lea.size());
    expect(matches(decode(start), start, std::array<uint8_t, 2>{7, 1}), "signed RIP-relative LEA still decodes");
    INSTRUX instruction{};
    expect(ND_SUCCESS(NdDecodeEx(&instruction, start, 8, ND_CODE_64, ND_DATA_64)) &&
        utility::resolve_displacement(reinterpret_cast<uintptr_t>(start), &instruction) ==
            reinterpret_cast<uintptr_t>(start - 32), "signed displacement resolution stays unchanged");
}

void test_reuse_and_threads(Mapping& memory) {
    auto* start = memory.data + 256;
    constexpr std::array<uint8_t, 4> code{0x90, 0x90, 0x90, 0xC3};
    std::memcpy(start, code.data(), code.size());
    for (int n = 0; n < 100; ++n) {
        expect(matches(decode(start), start, std::array<uint8_t, 4>{1, 1, 1, 1}),
            "repeated scans reuse and clear the existing seen table");
    }
    std::atomic<bool> good{true};
    std::vector<std::jthread> workers;
    for (int n = 0; n < 8; ++n) {
        workers.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                if (!matches(decode(start), start, std::array<uint8_t, 4>{1, 1, 1, 1})) {
                    good.store(false);
                }
            }
        });
    }
    workers.clear();
    expect(good.load(), "concurrent decoding retains independent thread-local state");
}
}

int main() {
    Mapping memory;
    if (!memory.data) {
        std::cerr << "Unable to allocate the protected-page fixture\n";
        return 1;
    }
    test_page_boundary(memory);
    test_readable_and_unreadable_pages(memory);
    test_existing_traversal(memory);
    test_reuse_and_threads(memory);
    std::cout << "Kananlib decode-window failures: " << failures << '\n';
    return failures ? 1 : 0;
}
