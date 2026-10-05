#include "mods/vr/SatisfactoryModular.hpp"
#include "SatisfactoryFamilyFixtures.hpp"

void test_satisfactory_array_snapshots() {
    namespace s = uevr::satisfactory;
    using Snapshot = s::ViewArraySnapshot;
    static_assert(std::is_trivially_destructible_v<Snapshot>);
    static_assert(sizeof(Snapshot) == sizeof(sdk::TArray<sdk::FSceneView*>));
    static_assert(!std::is_trivially_destructible_v<sdk::TArray<sdk::FSceneView*>>);

    std::array<uintptr_t, 2> objects{0x11111111, 0x22222222};
    const std::array<sdk::FSceneView*, 2> entries{
        reinterpret_cast<sdk::FSceneView*>(&objects[0]), reinterpret_cast<sdk::FSceneView*>(&objects[1])};
    auto source_entries = entries;
    auto clone_entries = entries;
    const Snapshot source{source_entries.data(), 2, 2};
    const Snapshot clone{clone_entries.data(), 2, 2};
    // AllViews may have allocated storage despite a zero count. Rejections and
    // scope exit must not free either array, including such empty allocations.
    const Snapshot empty_source{source_entries.data(), 0, 2};
    const Snapshot empty_clone{clone_entries.data(), 0, 2};
    const std::array descriptors{source, empty_source, clone, empty_clone};
    for (size_t exit_after = 0; exit_after <= descriptors.size(); ++exit_after) {
        const auto validate = [&]() {
            std::array<Snapshot, 4> snapshots{};
            for (size_t index = 0; index < exit_after; ++index) {
                std::memcpy(&snapshots[index], &descriptors[index], sizeof(Snapshot));
                expect(snapshots[index].data == descriptors[index].data &&
                    snapshots[index].count == descriptors[index].count &&
                    snapshots[index].capacity == descriptors[index].capacity,
                    "family array snapshots preserve borrowed ABI fields without taking ownership");
            }
        };
        validate();
        expect(source_entries == entries && clone_entries == entries &&
            objects == std::array<uintptr_t, 2>{0x11111111, 0x22222222},
            "source/clone buffers and views survive success and every partial-validation scope exit");
    }
}

void test_satisfactory_native_family() {
    namespace s = uevr::satisfactory;
    namespace f = satisfactory_family_fixture;
    struct Fixture {
        uintptr_t base;
        std::array<uint8_t, 0x4000> bytes{};
        bool readable{true}, executable{true};
        explicit Fixture(uintptr_t address) : base(address) {
            std::copy(f::family_copy.begin(), f::family_copy.end(), bytes.begin() + 0x1000);
            std::copy(f::family_destroy.begin(), f::family_destroy.end(), bytes.begin() + 0x2000);
            std::copy(f::eye_query.begin(), f::eye_query.end(), bytes.begin() + 0x2300);
            std::copy(f::secondary_query.begin(), f::secondary_query.end(), bytes.begin() + 0x2400);
            for (const auto instruction : {0x1022U, 0x2006U}) {
                const int32_t displacement = 0x3000 - instruction - 7;
                std::memcpy(bytes.data() + instruction + 3, &displacement, sizeof(displacement));
            }
            const auto deleting_destructor = base + 0x2700;
            std::memcpy(bytes.data() + 0x3000, &deleting_destructor, sizeof(deleting_destructor));
        }
        sdk::discovery::Memory memory() {
            return {this,
                [](void* context, uintptr_t address, void* out, size_t size) {
                    const auto& f = *static_cast<Fixture*>(context);
                    if (!f.readable || address < f.base || address - f.base >= f.bytes.size() ||
                        size > f.bytes.size() - (address - f.base)) { return false; }
                    std::memcpy(out, f.bytes.data() + (address - f.base), size); return true;
                },
                [](void* context, uintptr_t address, size_t size) {
                    const auto& f = *static_cast<Fixture*>(context);
                    return f.executable && address >= f.base + 0x1000 && address < f.base + 0x2800 &&
                        size <= f.base + 0x2800 - address;
                }};
        }
        std::optional<s::FamilyFunctions> validate() {
            return s::family_contract(memory(), {base, bytes.size()}, base + 0x1000, f::family_copy.size(),
                base + 0x2000, f::family_destroy.size(), base + 0x2300, base + 0x2400);
        }
    };
    for (const auto base : {uintptr_t{0x180000}, uintptr_t{0x7FFD12340000}}) {
        Fixture fixture{base};
        const auto original = fixture.bytes;
        const auto functions = fixture.validate();
        expect(functions && functions->copy == base + 0x1000 && functions->destroy == base + 0x2000 && functions->vtable == base + 0x3000,
            "matching native family copy/destructor and view metadata validate after relocation");
        expect(fixture.bytes == original, "family capability discovery never executes or modifies engine code");
        for (const auto offset : {0x1000U,0x101CU,0x1029U,0x107CU,0x137CU,0x138AU,0x1398U,0x13A6U,0x13FFU,0x1423U,0x1441U,
                 0x2000U,0x200DU,0x2016U,0x202CU,0x2042U,0x2058U,0x2302U,0x2429U,0x2478U}) {
            auto changed = fixture; changed.bytes[offset] ^= 1;
            expect(!changed.validate(), "changed allocation/owned-interface/view layout fails closed before a native call");
        }
        auto changed = fixture; changed.bytes[0x2009] ^= 8;
        expect(!changed.validate(), "constructor/destructor must agree on the same base family vtable");
        changed = fixture;
        const auto foreign_destructor = base + 0x5000;
        std::memcpy(changed.bytes.data() + 0x3000, &foreign_destructor, sizeof(foreign_destructor));
        expect(!changed.validate(), "foreign family destructor cannot establish Engine ownership");
        changed = fixture; changed.readable = false;
        expect(!changed.validate(), "unreadable Engine evidence cannot create a clone");
        changed = fixture; changed.executable = false;
        expect(!changed.validate(), "non-executable exports cannot create a clone");
        for (const auto size : {size_t{0}, f::family_copy.size() - 1, f::family_copy.size() + 1}) {
            expect(!s::family_contract(fixture.memory(), {base, fixture.bytes.size()}, base + 0x1000, size,
                base + 0x2000, f::family_destroy.size(), base + 0x2300, base + 0x2400), "unproven copy extent cannot write bounded family storage");
        }
        expect(!s::family_contract(fixture.memory(), {base, fixture.bytes.size()}, base + 0x1000, f::family_copy.size(),
            base + 0x2000, f::family_destroy.size() - 1, base + 0x2300, base + 0x2400), "truncated destructor contract fails closed");
    }

    std::array<uintptr_t, 4> borrowed{0x50000, 0, 0, 0};
    expect(s::fresh_upscalers(borrowed), "screen percentage may be borrowed before renderer extensions install upscalers");
    for (size_t index = 1; index < 4; ++index) {
        auto attached = borrowed; attached[index] = 0x60000;
        expect(!s::fresh_upscalers(attached), "a family with an attached upscaler cannot be resubmitted as a fresh input");
    }
    alignas(16) std::array<uint8_t, s::family_size> family{};
    const std::array<uintptr_t, 4> installed{0x50000, 0x70000, 0x80000, 0x90000};
    std::memcpy(family.data() + s::family_interfaces_offset, installed.data(), sizeof(installed));
    const auto before = family;
    s::release_borrowed_interfaces(family.data(), borrowed);
    std::array<uintptr_t, 4> released{};
    std::memcpy(released.data(), family.data() + s::family_interfaces_offset, sizeof(released));
    expect(released == std::array<uintptr_t, 4>{0, 0x70000, 0x80000, 0x90000},
        "clone teardown detaches source-owned screen percentage but destroys newly installed upscalers");
    expect(std::equal(family.begin(), family.begin() + s::family_interfaces_offset, before.begin()) &&
        std::equal(family.begin() + s::family_interfaces_offset + sizeof(installed), family.end(), before.begin() + s::family_interfaces_offset + sizeof(installed)),
        "borrowed ownership cleanup cannot modify arrays, flags or other fields");
    const auto own_borrowed = borrowed;
    expect(s::record_shared_interfaces({0x50000, 0xA0000, 0, 0}, installed, borrowed) && borrowed == own_borrowed,
        "independent family upscalers retain their correct destructor ownership");
    expect(!s::record_shared_interfaces(installed, installed, borrowed) && borrowed == installed,
        "unexpected shared upscalers reject publication and are detached before clone destruction");
    std::memcpy(family.data() + s::family_interfaces_offset, installed.data(), sizeof(installed));
    s::release_borrowed_interfaces(family.data(), borrowed);
    std::memcpy(released.data(), family.data() + s::family_interfaces_offset, sizeof(released));
    expect(released == std::array<uintptr_t, 4>{}, "rejection cannot double-delete any interface owned by the original family");

    std::array<int, 18> objects{};
    std::array<int*, 17> linked{};
    std::array<int*, 16> original{};
    for (size_t index = 0; index < original.size(); ++index) { original[index] = &objects[index]; }
    const auto count = s::link_families(std::span{original}, &objects[7], &objects[16], linked);
    expect(count == 17 && linked[7] == original[7] && linked[8] == &objects[16] && linked[16] == original[15],
        "one linked render preserves every auxiliary family and inserts only the distinct right-eye family");
    auto duplicates = original; duplicates[8] = &objects[7];
    expect(!s::link_families(std::span{duplicates}, &objects[7], &objects[16], linked), "duplicate selected families cannot overflow linked storage");
    expect(!s::link_families(std::span{original}, &objects[17], &objects[16], linked), "absent selected family cannot publish a transaction");
    expect(!s::link_families(std::span{original}, &objects[7], &objects[8], linked), "a right family already in the renderer list is rejected");
    expect(!s::link_families(std::span{original}, &objects[7], &objects[7], linked), "the same family cannot serve both render inputs");
    std::array<int*, 16> undersized{};
    expect(!s::link_families(std::span{original}, &objects[7], &objects[16], undersized), "insufficient family storage fails closed");
    expect(!s::link_families(std::span<int*>{}, &objects[7], &objects[16], linked), "empty input cannot create a renderer transaction");
}

void test_satisfactory_modular_discovery() {
    namespace s = uevr::satisfactory;
    constexpr std::wstring_view egs = L"D:\\Games\\Satisfactory\\Engine\\Binaries\\Win64\\FactoryGameEGS-Win64-Shipping.exe";
    constexpr std::wstring_view steam = L"D:\\Steam\\Satisfactory\\Engine\\Binaries\\Win64\\FactoryGameSteam-Win64-Shipping.exe";
    for (const auto path : {egs, steam, std::wstring_view{L"FactoryGameSteam-Win64-Shipping.exe"},
             std::wstring_view{L"C:/Games/FACTORYGAMEEGS-WIN64-SHIPPING.EXE"}}) {
        expect(s::supported_runtime(path, 0x50006, 0x10000), "Satisfactory EGS/Steam shipping names and UE5.6.1 are accepted");
        expect(s::supported_runtime(path, 0x50006, 0x10037), "Satisfactory compatible build revision is not locked to a changelist");
        for (uint32_t ms : {0x4001bu, 0x50005u, 0x50007u, 0x50008u, 0u}) {
            expect(!s::supported_runtime(path, ms, 0x10000), "Satisfactory other engine minors are unchanged");
        }
        for (uint32_t ls : {0u, 0x20000u, 0x30000u}) {
            expect(!s::supported_runtime(path, 0x50006, ls), "Satisfactory unvalidated engine patches are unchanged");
        }
    }
    for (const auto path : {L"", L"FactoryGame.exe", L"FactoryGameEGS-Win64-Shipping.exe.bak",
             L"NotFactoryGameSteam-Win64-Shipping.exe", L"FactoryGameSteam-WinGDK-Shipping.exe",
             L"D:\\FactoryGameEGS-Win64-Shipping.exe\\Other.exe", L"Townfall-Win64-Shipping.exe",
             L"Dungeons-Win64-Shipping.exe", L"Stalker2-Win64-Shipping.exe", L"NewTrinity.exe"}) {
        expect(!s::supported_runtime(path, 0x50006, 0x10000), "unrelated titles and substring lookalikes cannot select Satisfactory repairs");
    }
    expect(s::owns_module(egs, L"d:/games/satisfactory/engine/binaries/win64/FACTORYGAMEEGS-RENDERER-WIN64-SHIPPING.DLL", L"Renderer"),
        "Satisfactory Renderer module accepts Windows case/separator equivalents");
    expect(s::owns_module(steam, L"D:\\Steam\\Satisfactory\\Engine\\Binaries\\Win64\\FactoryGameSteam-Core-Win64-Shipping.dll", L"Core"),
        "Steam console route requires the matching Steam Core module");
    for (const auto path : {L"D:\\Games\\Satisfactory\\Engine\\Binaries\\Win64\\FactoryGameSteam-Renderer-Win64-Shipping.dll",
             L"D:\\Games\\Satisfactory\\Engine\\Binaries\\Win64\\FactoryGameEGS-Engine-Win64-Shipping.dll",
             L"D:\\Other\\FactoryGameEGS-Renderer-Win64-Shipping.dll",
             L"D:\\Games\\Satisfactory\\Engine\\Binaries\\Win64\\FactoryGameEGS-Renderer-Win64-Shipping.dll.bak",
             L"FactoryGameEGS-Renderer-Win64-Shipping.dll"}) {
        expect(!s::owns_module(egs, path, L"Renderer"), "mixed flavor, foreign directory, wrong component or renamed DLL fails closed");
    }
    for (unsigned mask = 0; mask < 8; ++mask) {
        expect(s::use_modular_renderer(mask & 1, mask & 2, mask & 4) == (mask == 7),
            "modular renderer ownership is enabled only for validated Satisfactory DX12 Native Fix");
    }
    expect(s::owns_renderer_pair(0x10000, 0x10000, 0x10000), "renderer caller and callee must share the validated DLL");
    expect(!s::owns_renderer_pair(0, 0, 0) && !s::owns_renderer_pair(0x10000, 0x20000, 0x10000) &&
        !s::owns_renderer_pair(0x10000, 0x10000, 0x20000), "no null, EXE, mixed or unrelated DLL ownership");
    for (const auto name : {L"r.OneFrameThreadLag", L"r.AllowOcclusionQueries", L"r.VolumetricCloud", L"r.AmbientOcclusionLevels",
             L"r.DepthOfFieldQuality", L"r.MotionBlurQuality", L"r.SceneColorFringeQuality", L"r.DefaultFeature.AmbientOcclusion",
             L"r.TemporalAA.Upsampling", L"r.ScreenPercentage"}) {
        expect(s::recover_console_variable(name), "source-confirmed misrouted CVar selects the guarded interface");
    }
    for (const auto name : {L"", L"r.SSGI.Enable", L"r.TemporalAA.Algorithm", L"r.DefaultFeature.AntiAliasing",
             L"r.PostProcessing.PropagateAlpha", L"r.RayTracing", L"r.ScreenPercentage.Extra", L"Game.Command"}) {
        expect(!s::recover_console_variable(name), "unrelated controls and deprecated/absent aliases keep the previous path");
    }

    s::ConsoleRetry retry;
    expect(retry.begin(0) && retry.attempts == 1, "first CVar lookup is immediate");
    for (int64_t now = 1; now < s::ConsoleRetry::interval_ms; ++now) {
        expect(!retry.begin(now) && retry.attempts == 1, "failed CVar discovery cannot rescan every frame");
    }
    for (uint32_t attempt = 1; attempt < s::ConsoleRetry::max_attempts; ++attempt) {
        expect(retry.begin(attempt * s::ConsoleRetry::interval_ms), "bounded CVar retries remain available after warmup");
    }
    expect(!retry.begin(100000) && retry.attempts == s::ConsoleRetry::max_attempts, "per-entry retry cap stops permanent misses");
    expect(!s::ConsoleRetry{}.begin(-1) && !s::ConsoleRetry{}.begin((std::numeric_limits<int64_t>::max)()),
        "invalid/overflowing retry timestamps fail closed");
    expect(s::console_array_bytes(0x40000, 7124, 8192) == 7124U * 32U,
        "observed modular console array has a bounded snapshot size");
    expect(s::console_array_bytes(0x40000, 65536, 65536) == 65536U * 32U,
        "bounded console registry permits its exact supported upper limit");
    for (const auto address : {uintptr_t{0}, uintptr_t{0x40001}, (std::numeric_limits<uintptr_t>::max)() - 7}) {
        expect(!s::console_array_bytes(address, 1, 1), "null, unaligned or overflowing registry pointers are rejected");
    }
    expect(!s::console_array_bytes(0x40000, 0, 8192) && !s::console_array_bytes(0x40000, 8193, 8192) &&
        !s::console_array_bytes(0x40000, 7124, 65537), "empty, corrupt or excessive console arrays cannot trigger unbounded walks");
    const s::Image image{0x20000, 0x10000};
    expect(image.contains(0x20000, 1) && image.contains(0x2fff8, 8), "module ranges include the exact final byte");
    expect(!image.contains(0x1ffff, 1) && !image.contains(0x30000, 1) && !image.contains(0x2fff8, 9) && !image.contains(0x20000, 0),
        "module range checks reject truncation, underflow and empty evidence");
    expect(!s::Image{(std::numeric_limits<uintptr_t>::max)() - 3, 8}.contains((std::numeric_limits<uintptr_t>::max)() - 3, 1),
        "overflowing image range cannot validate a callable address");

    struct ConsoleFixture {
        std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000);
        uintptr_t table = 0x22000;
        std::array<uintptr_t, 26> slots{};
        std::array<uintptr_t, 8> code{0x23000,0x23100,0x23200,0x23300,0x23400,0x23500,0x23600,0x23700};
        bool executable = true;
        bool readable_table = true;
        bool readable_object = true;

        ConsoleFixture() {
            const std::array<unsigned, 8> indexes{0,7,9,16,20,21,24,25};
            for (size_t i = 0; i < indexes.size(); ++i) { slots[indexes[i]] = code[i]; }
            slots[10] = 0x23800;
            for (auto address : code) { bytes[address - 0x20000] = 0xC3; }
            write(code[1], std::array<uint8_t,4>{0x48,0x8B,0xC1,0xC3});
            write(code[2], std::array<uint8_t,3>{0xB0,0x01,0xC3});
            write(code[3], std::array<uint8_t,3>{0x33,0xC0,0xC3});
            write(0x23800, std::array<uint8_t,3>{0x32,0xC0,0xC3});
            write(code[5], std::array<uint8_t,24>{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,
                0x57,0x48,0x83,0xEC,0x20,0x41,0x8B,0xF8,0x48,0x8B,0xF2,0x48,0x8B,0xD9});
            publish();
        }
        void write(uintptr_t address, std::span<const uint8_t> value) {
            std::memcpy(bytes.data() + (address - 0x20000), value.data(), value.size());
        }
        void publish() { std::memcpy(bytes.data() + 0x2000, slots.data(), sizeof(slots)); }
        sdk::discovery::Memory memory() {
            return {this,
                [](void* context, uintptr_t address, void* out, size_t size) {
                    const auto& f = *static_cast<ConsoleFixture*>(context);
                    if (address == 0x40000 && size == sizeof(f.table)) {
                        if (!f.readable_object) { return false; }
                        std::memcpy(out, &f.table, size); return true;
                    }
                    if (address == 0x22000 && !f.readable_table) { return false; }
                    if (address < 0x20000 || address - 0x20000 >= f.bytes.size() || size > f.bytes.size() - (address - 0x20000)) { return false; }
                    std::memcpy(out, f.bytes.data() + (address - 0x20000), size); return true;
                },
                [](void* context, uintptr_t address, size_t size) {
                    const auto& f = *static_cast<ConsoleFixture*>(context);
                    return f.executable && address >= 0x23000 && address < 0x23900 && size <= 0x23900 - address;
                }};
        }
    };
    ConsoleFixture fixture;
    const auto original_bytes = fixture.bytes;
    expect(s::validated_console_variable(fixture.memory(), image, 0x40000, s::ConsoleType::Integer), "UE5.6.1 numeric/ref CVar table validates without calls");
    expect(fixture.bytes == original_bytes, "CVar validation is read-only and never invokes the candidate");
    expect(!s::validated_console_variable(fixture.memory(), image, 0x40000, s::ConsoleType::Floating), "an int cannot silently become a float entry");
    auto floating = fixture;
    floating.write(floating.code[2], std::array<uint8_t,3>{0x32,0xC0,0xC3});
    floating.write(0x23800, std::array<uint8_t,3>{0xB0,0x01,0xC3});
    expect(s::validated_console_variable(floating.memory(), image, 0x40000, s::ConsoleType::Floating), "validated floating CVar retains GetFloat/Set behavior");
    expect(!s::validated_console_variable(floating.memory(), image, 0x40000, s::ConsoleType::Integer), "wrong numeric type fails closed");
    auto boolean = fixture;
    boolean.slots[8] = 0x23800; boolean.publish();
    boolean.write(boolean.code[2], std::array<uint8_t,3>{0x32,0xC0,0xC3});
    boolean.write(0x23800, std::array<uint8_t,3>{0xB0,0x01,0xC3});
    expect(s::validated_console_variable(boolean.memory(), image, 0x40000, s::ConsoleType::Boolean),
        "r.AllowOcclusionQueries is a source-proven bool ref, not raw two-slot integer data");
    expect(!s::validated_console_variable(boolean.memory(), image, 0x40000, s::ConsoleType::Integer),
        "bool refs are not misclassified as integer variable storage");
    for (auto address : {uintptr_t{0}, uintptr_t{0x40001}, (std::numeric_limits<uintptr_t>::max)()}) {
        expect(!s::validated_console_variable(fixture.memory(), image, address, s::ConsoleType::Integer), "bad object pointer cannot publish a CVar");
    }
    for (auto table : {uintptr_t{0}, uintptr_t{0x22001}, uintptr_t{0x2fff8}, uintptr_t{0x40000}}) {
        auto bad = fixture; bad.table = table;
        expect(!s::validated_console_variable(bad.memory(), image, 0x40000, s::ConsoleType::Integer), "unaligned, truncated or foreign table fails closed");
    }
    for (auto index : {0U,7U,9U,16U,20U,21U,24U,25U}) {
        auto bad = fixture; bad.slots[index] = 0x41000; bad.publish();
        expect(!s::validated_console_variable(bad.memory(), image, 0x40000, s::ConsoleType::Integer), "every required function must belong to executable Core image code");
    }
    for (auto index : {1U,2U,3U,5U}) {
        auto bad = fixture; bad.bytes[bad.code[index] - 0x20000] ^= 1;
        expect(!s::validated_console_variable(bad.memory(), image, 0x40000, s::ConsoleType::Integer), "changed casts/type/tagged-Set ABI do not publish an interface");
    }
    auto bad = fixture; bad.executable = false;
    expect(!s::validated_console_variable(bad.memory(), image, 0x40000, s::ConsoleType::Integer), "non-executable vfunc cannot be invoked");
    bad = fixture; bad.readable_table = false;
    expect(!s::validated_console_variable(bad.memory(), image, 0x40000, s::ConsoleType::Integer), "unreadable table fails closed");
    bad = fixture; bad.readable_object = false;
    expect(!s::validated_console_variable(bad.memory(), image, 0x40000, s::ConsoleType::Integer), "unreadable object fails closed");
    bad = fixture; std::swap(bad.slots[20], bad.slots[21]); bad.publish();
    expect(!s::validated_console_variable(bad.memory(), image, 0x40000, s::ConsoleType::Integer), "shifted UE5.5/5.7 Set slots are rejected");

    // Actual matching Renderer DLL wrapper, relocated rather than bound to an RVA.
    const std::array<uint8_t,53> wrapper{
        0x48,0x83,0xEC,0x38,0xC7,0x44,0x24,0x28,0x01,0x00,0x00,0x00,0x48,0x8D,0x44,0x24,
        0x50,0x48,0x89,0x44,0x24,0x20,0x0F,0x28,0x44,0x24,0x20,0x4C,0x89,0x44,0x24,0x50,
        0x4C,0x8D,0x44,0x24,0x20,0x66,0x0F,0x7F,0x44,0x24,0x20,0xE8,0x60,0xED,0xFF,0xFF,
        0x48,0x83,0xC4,0x38,0xC3};
    constexpr uintptr_t begin = 0x110000, caller_return = begin + 48, callee = begin - 0x1270;
    expect(s::one_family_wrapper(wrapper, begin, callee, caller_return), "PDB-confirmed one-family renderer wrapper validates after ASLR");
    expect(!s::one_family_wrapper(wrapper, begin, callee + 1, caller_return) &&
        !s::one_family_wrapper(wrapper, begin, callee, caller_return + 1), "wrong call destination/return cannot identify a renderer");
    for (auto index : {7U,8U,16U,21U,31U,36U,44U,52U}) {
        auto changed = wrapper; changed[index] ^= 1;
        expect(!s::one_family_wrapper(changed, begin, callee, caller_return), "changed count, stack array, call or return fails closed");
    }
    expect(!s::one_family_wrapper(std::span{wrapper}.first(52), begin, callee, caller_return), "truncated wrapper cannot publish a target");
    expect(!s::one_family_wrapper(wrapper, (std::numeric_limits<uintptr_t>::max)() - 20, callee, caller_return),
        "wrapper address overflow cannot be accepted");
    std::vector<uint8_t> oversized(0x181, 0x90);
    expect(!s::one_family_wrapper(oversized, begin, callee, caller_return), "large arbitrary frames cannot act as a DLL wrapper fallback");
}
