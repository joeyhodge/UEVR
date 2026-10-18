#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "mods/vr/GearsEDayAim.hpp"
#include <sdk/GearsEDayDiscovery.hpp>
#include <sdk/vtables/IXRTrackingSystemVTables.hpp>

namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

// Original dump-confirmed Draw instructions, not a generated decoder template.
constexpr std::array<uint8_t, 88> original{
    0x48,0x8b,0x0d,0x47,0x09,0x3e,0x09,0x48,0x8b,0x89,0xb0,0x10,0,0,
    0x48,0x85,0xc9,0x74,0x45,0x48,0x8b,0x01,0xff,0x90,0x18,0x01,0,0,
    0x48,0x85,0xc0,0x74,0x37,0x48,0x8b,0x05,0x26,0x09,0x3e,0x09,
    0x48,0x8b,0x88,0xb0,0x10,0,0,0x48,0x8b,0x01,0xff,0x90,0x18,0x01,0,0,
    0x48,0x8d,0x94,0x24,0xa8,0x02,0,0,0x48,0x8b,0xc8,0x4c,0x8b,0x00,
    0x41,0xff,0x90,0xe0,0,0,0,0x48,0x8b,0x08,0x48,0x89,0x8c,0x24,0xf0,0,0,0
};

struct Fixture {
    static constexpr uintptr_t base = 0x100000;
    static constexpr uintptr_t global = 0x200000;
    static constexpr uintptr_t engine = 0x300000;
    static constexpr size_t candidate = 0x15a;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x285, 0x90);
    uintptr_t stored_engine{engine};
    bool readable{true}, executable{true};
    size_t reads{};
    Fixture() { std::memcpy(bytes.data() + candidate, original.data(), original.size()); relocate(candidate); }
    void u32(size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); }
    void relocate(size_t at) {
        u32(at + 3, uint32_t(global - (base + at + 7)));
        u32(at + 36, uint32_t(global - (base + at + 40)));
    }
    sdk::discovery::Memory memory() {
        return {this,
            [](void* context, uintptr_t address, void* out, size_t size) {
                auto& f = *static_cast<Fixture*>(context);
                ++f.reads;
                if (!f.readable) { return false; }
                if (address == global && size == sizeof(uintptr_t)) {
                    std::memcpy(out, &f.stored_engine, size); return true;
                }
                if (address < base || address - base > f.bytes.size() || size > f.bytes.size() - (address - base)) { return false; }
                std::memcpy(out, f.bytes.data() + (address - base), size);
                return true;
            },
            [](void* context, uintptr_t address, size_t size) {
                const auto& f = *static_cast<Fixture*>(context);
                return f.executable && address >= base && address - base <= f.bytes.size() &&
                    size <= f.bytes.size() - (address - base);
            }};
    }
    auto discover(size_t size = 0) {
        return uevr::gears_aim::debug_canvas_query(memory(), base, size ? size : bytes.size(), engine, 0x10b0, 35);
    }
};

void test_discovery() {
    Fixture f;
    const auto before = f.bytes;
    const auto query = f.discover();
    expect(query && query->slot == 28 && query->return_address == f.base + f.candidate + 77,
        "original E-Day Draw proves slot 28 and the exact failing return address");
    expect(f.bytes == before && f.reads == 2, "discovery performs bounded reads and no code or data writes");
    f.u32(f.candidate + 73, 27 * 8);
    expect(f.discover() && f.discover()->slot == 27, "a source-compatible stock slot is not shifted");
    f.u32(f.candidate + 60, 0x380);
    f.u32(f.candidate + 84, 0x100);
    expect(bool(f.discover()), "valid stack displacement changes do not require an EXE address manifest");

    for (size_t i = 0; i < original.size(); ++i) {
        if (uevr::gears_aim::debug_canvas_mask[i] == 0) { continue; }
        Fixture mutation;
        mutation.bytes[mutation.candidate + i] ^= 1;
        expect(!mutation.discover(), "changed receiver, sret, call or result-consumer instructions reject");
    }
    for (const auto offset : {10,43,24,52,73,60,84}) {
        Fixture mutation;
        mutation.u32(mutation.candidate + offset, 0xffffffff);
        expect(!mutation.discover(), "wrong engine/getter/size slot or invalid stack buffer rejects");
    }
    for (const auto offset : {18,32,36}) {
        Fixture mutation;
        mutation.bytes[mutation.candidate + offset] ^= 8;
        expect(!mutation.discover(), "divergent null branch or second engine global rejects");
    }
    Fixture overlap;
    overlap.u32(overlap.candidate + 84, 0x2a8);
    expect(!overlap.discover(), "aliased hidden output and copied result reject");
    Fixture wrong_owner;
    ++wrong_owner.stored_engine;
    expect(!wrong_owner.discover(), "a different engine owner rejects");
    Fixture ambiguous;
    const auto second = ambiguous.bytes.size();
    ambiguous.bytes.resize(second + original.size(), 0x90);
    std::memcpy(ambiguous.bytes.data() + second, original.data(), original.size());
    ambiguous.relocate(second);
    expect(!ambiguous.discover(), "multiple possible query sites reject before exposing XRSystem");
    Fixture outside;
    outside.bytes.resize(0x1100, 0x90);
    std::fill(outside.bytes.begin(), outside.bytes.end(), 0x90);
    std::memcpy(outside.bytes.data() + 0x1000, original.data(), original.size());
    outside.relocate(0x1000);
    expect(!outside.discover(), "discovery never scans beyond its bounded original Draw window");
    Fixture unreadable;
    unreadable.readable = false;
    expect(!unreadable.discover(), "failed memory read rejects");
    unreadable.readable = true;
    unreadable.executable = false;
    expect(!unreadable.discover(), "non-executable source rejects");
    Fixture truncated;
    expect(!truncated.discover(truncated.candidate + original.size() - 1), "truncated original sequence rejects");
    expect(!uevr::gears_aim::debug_canvas_query(truncated.memory(), UINTPTR_MAX - 16, 0x200,
        truncated.engine, 0x10b0, 35), "overflowing code span rejects");
}

int32_t* null_stub(void*, int32_t*) { return nullptr; }
int32_t* size_stub(void*, int32_t* out) { return uevr::gears_aim::write_debug_canvas_size(out); }

void test_typed_return() {
    using Call = int32_t*(*)(void*, int32_t*);
    std::array<Call, 200> table{};
    table.fill(null_stub);
    table[27] = size_stub;
    const auto original_table = table;
    Fixture f;
    const auto query = f.discover();
    std::array<int32_t, 4> guarded{11, 0, 0, 22};
    expect(table[28](nullptr, guarded.data() + 1) == nullptr,
        "old stock-slot-only table reproduces the null return despite a valid hidden output");
    if (query) { table[query->slot] = size_stub; }
    const auto result = table[28](nullptr, guarded.data() + 1);
    expect(result == guarded.data() + 1 && result[0] == 1024 && result[1] == 1024,
        "validated virtual dispatch returns initialized FIntPoint storage, not a null or register-value substitute");
    expect(guarded[0] == 11 && guarded[3] == 22, "callback writes exactly eight bytes");
    expect(size_stub(nullptr, nullptr) == nullptr, "null hidden output is not dereferenced");
    for (size_t slot = 0; slot < table.size(); ++slot) {
        expect(slot == 28 || table[slot] == original_table[slot], "unrelated virtual methods remain unchanged");
    }
    for (int iteration = 0; iteration < 1000; ++iteration) {
        expect(table[28](nullptr, guarded.data() + 1) == guarded.data() + 1, "repeated debug draws remain allocation-free");
    }
}

void test_scope() {
    namespace g = sdk::gears_eday;
    expect(g::eligible(L"D:\\Game\\Win64\\GoWEDay-Steam.exe", 0x50006, 0x10000), "known Steam identity admits validation");
    expect(g::eligible(L"D:\\Game\\WinGDK\\GoWEDay.exe", 0x50006, 0x10000), "GDK identity still requires its own code proof");
    expect(!g::eligible(L"D:\\Other\\GoWEDay.exe", 0x50006, 0x10000), "unrelated GoWEDay basename rejects");
    expect(!g::eligible(L"D:\\Game\\GoWEDay-Steam.exe", 0x50007, 0x10000), "different engine interval rejects");
    expect(!g::eligible(L"D:\\Game\\Other.exe", 0x50006, 0x10000), "other UE5.6.1 titles do not enter this correction");
    expect(ue5_6::IHeadMountedDisplayVT::get().GetIdealDebugCanvasRenderTargetSize_index() == 27,
        "stock UE5.6 vtable remains unchanged");
    expect(ue5_6::IHeadMountedDisplayVT::get().GetDistortionScalingFactor_index() == 28,
        "stock distortion slot is not globally overwritten");
}

void test_saved_draw(const char* filename) {
    std::ifstream file(filename, std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{file}, {}};
    expect(file.is_open() && bytes.size() == 0x285, "original saved Draw body is present and complete");
    if (bytes.size() != 0x285) { return; }
    Fixture f;
    f.bytes = bytes;
    expect(std::memcmp(f.bytes.data() + f.candidate, original.data(), original.size()) == 0,
        "test fixture matches the saved game code independently");
    f.relocate(f.candidate);
    const auto query = f.discover();
    expect(query && query->slot == 28 && query->return_address - f.base == 0x1a7,
        "complete original game function discovers the dump-confirmed ABI without calling game code");
}
}

int main(int argc, char** argv) {
    test_discovery();
    test_typed_return();
    test_scope();
    if (argc == 3 && std::string_view{argv[1]} == "--draw") { test_saved_draw(argv[2]); }
    std::cout << "Gears E-Day aim ABI tests: " << failures << " failures\n";
    return failures ? 1 : 0;
}
