#include "mods/vr/DuneSmartGI.hpp"

void test_dune_smartgi() {
    namespace s = uevr::dune_smartgi;
    auto code = s::getter_code;
    expect(s::valid_getter(code), "Dune complete CPU getter contract is accepted");
    for (size_t i = 0; i < code.size(); ++i) {
        auto changed = code; changed[i] ^= 1;
        expect(s::valid_getter(changed) == uevr::dune_frame::displacement_byte(i, s::getter_displacements),
            "only the verified constant displacement can relocate, not getter layout or arithmetic");
    }
    expect(!s::valid_getter(std::span{code}.first(code.size() - 1)), "truncated getter is rejected");
    constexpr uintptr_t getter_entry = 0x140000000;
    auto segment_length = [&](uintptr_t at) -> std::optional<size_t> {
        if (at == getter_entry) { return 0x3A9; }
        if (at == getter_entry + 0x3A9) { return 0x176; }
        if (at == getter_entry + 0x51F) { return 0xB; }
        return {};
    };
    auto owns_getter = [&](uintptr_t owner, uintptr_t, size_t) { return owner == getter_entry; };
    expect(s::valid_getter_unwind(getter_entry, segment_length, owns_getter), "all three chained getter segments belong to the same entry");
    expect(!s::valid_getter_unwind(getter_entry, segment_length, [](uintptr_t, uintptr_t, size_t) { return false; }),
        "adjacent but differently owned functions cannot become a getter");
    expect(!s::valid_getter_unwind(getter_entry, [](uintptr_t) { return std::optional<size_t>{0x52B}; }, owns_getter),
        "oversized unwind body rejected");
    expect(!s::valid_getter_unwind(getter_entry, [](uintptr_t) { return std::optional<size_t>{1}; }, owns_getter),
        "fragmented unwind traversal is bounded");
    expect(!s::valid_getter_unwind(getter_entry, [](uintptr_t) { return std::optional<size_t>{0}; }, owns_getter),
        "zero-length unwind cannot loop");
    {
        constexpr uintptr_t base = 0x140000000, entry = base + 0x1000;
        std::vector<uint8_t> metadata(0x5000);
        std::array functions{s::UnwindFunction{0x1000, 0x13A9, 0x4000},
            s::UnwindFunction{0x13A9, 0x151F, 0x4100}, s::UnwindFunction{0x151F, 0x152A, 0x4200}};
        auto put = [&](size_t offset, const auto& value) { std::memcpy(metadata.data() + offset, &value, sizeof(value)); };
        put(0x4000, std::array<uint8_t, 4>{1, 0, 0, 0});
        put(0x4100, std::array<uint8_t, 4>{0x21, 0, 3, 0}); put(0x410C, functions[0]);
        put(0x4200, std::array<uint8_t, 4>{0x21, 0, 0, 0}); put(0x4204, functions[0]);
        const auto original = metadata;
        const auto registered = functions;
        uintptr_t blocked{};
        auto lookup = [&](uintptr_t at) -> std::optional<s::UnwindFunction> {
            for (const auto fn : functions) {
                if (at >= base + fn.begin && at < base + fn.end) { return fn; }
            }
            return {};
        };
        auto read = [&](uintptr_t at, void* out, size_t size) {
            if (at < base || at - base > metadata.size() || size > metadata.size() - (at - base) ||
                (blocked >= at && blocked - at < size)) { return false; }
            std::memcpy(out, metadata.data() + at - base, size); return true;
        };
        auto owns = [&](uintptr_t at) { return s::getter_segment_owner({base, metadata.size()}, entry, at, read, lookup); };
        for (const auto fn : functions) { expect(owns(base + fn.begin), "root, loop and disjoint epilogue each prove their registered root"); }
        for (const uint8_t header : {uint8_t{0}, uint8_t{2}, uint8_t{9}, uint8_t{0x19}, uint8_t{0x29}}) {
            metadata[0x4100] = header;
            expect(!owns(base + 0x13A9), "invalid version or exception-handler flags cannot stand in for CHAININFO"); metadata = original;
        }
        for (const auto parent : {s::UnwindFunction{0x13A9, 0x151F, 0x4100},
                 s::UnwindFunction{0x1000, 0x13AA, 0x4000}, s::UnwindFunction{0x1000, 0x13A9, 0x4300}}) {
            put(0x410C, parent);
            expect(!owns(base + 0x13A9), "cyclic, overlapping or unregistered chained metadata rejects the getter"); metadata = original;
        }
        put(0x4204, s::UnwindFunction{0x13A9, 0x151F, 0x4100});
        expect(owns(base + 0x151F), "a bounded registered intermediate chain is accepted too"); metadata = original;
        blocked = base + 0x4204;
        expect(!owns(base + 0x151F), "unreadable chained parent cannot establish ownership"); blocked = 0;
        functions[1].unwind = 0x5000;
        expect(!owns(base + 0x13A9), "unwind headers outside this image cannot establish ownership"); functions = registered;
        functions[0].begin -= 8;
        expect(!owns(base + 0x13A9), "registered entry must equal the proven getter entry"); functions = registered;
        expect(!owns(entry + 1) && !owns(entry - 8) && !owns(entry + s::getter_code.size()),
            "only exact segment starts inside the complete getter are eligible");
        expect(!s::getter_segment_owner({base, 0x1100}, entry, entry, read, lookup), "truncated image rejects even a readable entry");
        expect(!s::getter_segment_owner({UINTPTR_MAX - 0x10, 0x100}, entry, entry, read, lookup), "overflowing image bounds rejected");
    }
    for (unsigned mask = 0; mask < 32; ++mask) {
        expect(s::enabled(mask & 1, mask & 2, mask & 4, mask & 8, mask & 16) == (mask == 31),
            "SmartGI repair requires validated DX12/OpenXR/active HMD/Native; other modes unchanged");
    }
    constexpr uintptr_t image_base = 0x140000000, constructor = image_base + 0x1000;
    std::vector<uint8_t> image_code(0x6000, 0x90);
    auto put = [&](size_t offset, const auto& bytes) { std::copy(bytes.begin(), bytes.end(), image_code.begin() + 0x1000 + offset); };
    put(0x49, uevr::dune_native::owner_code); put(0x3B8, uevr::dune_native::pass_code);
    put(0x496, uevr::dune_native::index_code); put(0x1003, uevr::dune_native::instanced_code);
    put(0x1057, uevr::dune_native::multiview_code); put(0x116F, uevr::dune_native::primary_code); put(0x11C9, s::exposure_code);
    const auto link = [&](size_t call, uintptr_t target) {
        const auto displacement = static_cast<int32_t>(target - (image_base + call + 5));
        std::memcpy(image_code.data() + call + 1, &displacement, sizeof(displacement));
    };
    link(0x1000 + 0x11C9 + 33, image_base + 0x4000); link(0x1000 + 0x11C9 + 64, image_base + 0x4100);
    std::copy(s::secondary_test.begin(), s::secondary_test.end(), image_code.begin() + 0x4000);
    std::copy(s::primary_test.begin(), s::primary_test.end(), image_code.begin() + 0x4100);
    const auto valid_image = image_code;
    auto code_read = [&](uintptr_t address, void* value, size_t size) {
        if (address < image_base || address - image_base > image_code.size() || size > image_code.size() - (address - image_base)) { return false; }
        std::memcpy(value, image_code.data() + address - image_base, size); return true;
    };
    const auto exposure_ok = [&] { return s::valid_exposure_constructor({image_base, image_code.size()}, constructor,
        std::span<const uint8_t>{image_code}.subspan(0x1000, 0x1300), code_read); };
    expect(exposure_ok(), "constructor's shared primary link and actual pass-test callees validate independently");
    for (size_t i = 0; i < s::exposure_code.size(); ++i) {
        image_code = valid_image; image_code[0x1000 + 0x11C9 + i] ^= 1;
        expect(!exposure_ok(), "changed exposure code or wrongly linked pass-test callee rejects repair");
    }
    for (size_t i = 0; i < s::secondary_test.size(); ++i) {
        image_code = valid_image; image_code[0x4000 + i] ^= 1;
        expect(!exposure_ok(), "constructor cannot borrow from a different stereo classification");
        image_code = valid_image; image_code[0x4100 + i] ^= 1;
        expect(!exposure_ok(), "constructor cannot borrow from a differently classified primary");
    }
    image_code = valid_image; put(0x200, s::exposure_code);
    expect(!exposure_ok(), "ambiguous exposure-link proof rejected");
    image_code = valid_image;
    expect(!s::valid_exposure_constructor({image_base, image_code.size()}, constructor,
        std::span<const uint8_t>{image_code}.subspan(0x1000, 0x1200), code_read), "truncated exposure link cannot borrow state");

    constexpr uintptr_t heap = 0x100000, family = heap + 0x100, data = heap + 0x200;
    constexpr uintptr_t left = heap + 0x1000, right = heap + 0x5000;
    constexpr uintptr_t primary = heap + 0x9000, secondary = heap + 0xC000, scene = heap + 0xF000, table = 0x200000;
    std::vector<uint8_t> memory(0x14000);
    auto write = [&](uintptr_t address, auto value) { std::memcpy(memory.data() + address - heap, &value, sizeof(value)); };
    uintptr_t unreadable{};
    auto read = [&](uintptr_t address, void* output, size_t size) {
        if (address < heap || address - heap > memory.size() || size > memory.size() - (address - heap) ||
            (unreadable >= address && unreadable - address < size)) { return false; }
        std::memcpy(output, memory.data() + address - heap, size); return true;
    };
    auto valid = [&](uintptr_t object) { uintptr_t t{}; return read(object, &t, sizeof(t)) && t == table; };
    for (const auto object : {family, left, right, primary, secondary, scene}) { write(object, table); }
    write(family + 8, data); write(family + 0x10, int32_t{2}); write(family + 0x14, int32_t{2});
    write(family + 0x28, scene); write(family + 0x84, uint32_t{100}); write(data, left); write(data + 8, right);
    for (const auto view : {left, right}) {
        write(view + 0x10, family); write(view + 0x18, view == left ? primary : secondary);
        write(view + 0x13A0, view == left ? int32_t{1} : int32_t{2});
        write(view + 0x13A4, view == left ? int32_t{0} : int32_t{1}); write(view + 0x13A8, int32_t{0});
        write(view + s::exposure_state_offset, primary); write(view + s::view_info_state_offset, view == left ? primary : secondary);
    }
    write(primary + s::dimensions_offset, std::array<int32_t, 3>{64, 64, 64});
    for (size_t i = 0; i < s::maximum_clipmaps; ++i) {
        const auto entry = primary + s::clipmaps_offset + i * s::clipmap_stride;
        write(entry, s::Vector3{-100.0 - i, 25.0 + i, 300.0 - i});
        write(entry + 0x18, s::Vector3{64.0 * (i + 1), 128.0 * (i + 1), 256.0 * (i + 1)});
        write(entry + 0x30, s::Vector3{2.0, 4.0, 8.0}); write(entry + 0x48, float{1.25});
    }
    write(primary + s::clipmap_count_offset, int32_t{3});
    const auto paired = memory;
    expect(s::missing_secondary_source(right, false, read, valid) == primary, "ordinary Native uses the authored current primary link");
    expect(s::missing_secondary_source(right, true, read, valid) == primary, "Native Fix warmup preserves the same original pair");
    expect(!s::missing_secondary_source(left, true, read, valid), "primary eye remains unchanged");
    for (const auto frame : {uint32_t{0}, UINT32_MAX}) {
        write(family + 0x84, frame);
        expect(s::missing_secondary_source(right, false, read, valid) == primary,
            "current authored state ownership does not depend on a wrapping frame counter");
    }
    memory = paired;
    for (int32_t n : {1, 4, 8}) {
        write(primary + s::clipmap_count_offset, n);
        const auto info = s::read_primary_clipmaps(primary, read);
        expect(info && info->count == n && info->origin[0].x == -100.0 && info->extent[0].y == 128.0 &&
            info->world_to_uv_scale[0].z == 0.5 / 256.0 && info->voxel_size_and_radius[0].w == 1.25 &&
            (n == 8 || info->voxel_size_and_radius[n].w == 1.0), "numeric clipmap layout and unused element defaults match the engine");
    }
    memory = paired;
    expect(memory == paired && s::read_primary_clipmaps(primary, read).has_value(), "clipmap repair preparation never mutates view, exposure or state memory");
    unsigned numeric_reads{};
    auto counted = [&](uintptr_t at, void* out, size_t size) { ++numeric_reads; return read(at, out, size); };
    expect(s::read_primary_clipmaps(primary, counted).has_value() && numeric_reads == 6,
        "three clipmaps need only one bounded numeric-prefix read each plus count/dimensions checks");
    for (int32_t n : {-1, 0, 9, INT32_MAX}) {
        write(primary + s::clipmap_count_offset, n);
        expect(!s::read_primary_clipmaps(primary, read), "invalid clipmap count cannot overrun eight inline entries");
    }
    memory = paired;
    for (const double value : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        write(primary + s::clipmaps_offset + 0x18, value);
        expect(!s::read_primary_clipmaps(primary, read), "invalid extents cannot produce shader inputs"); memory = paired;
    }
    write(primary + s::clipmaps_offset, std::numeric_limits<double>::quiet_NaN());
    expect(!s::read_primary_clipmaps(primary, read), "nonfinite origins rejected"); memory = paired;
    write(primary + s::clipmaps_offset + 0x30, double{});
    expect(!s::read_primary_clipmaps(primary, read), "zero voxel size rejected"); memory = paired;
    write(primary + s::clipmaps_offset + 0x48, std::numeric_limits<float>::infinity());
    expect(!s::read_primary_clipmaps(primary, read), "nonfinite radius rejected"); memory = paired;
    for (int32_t dimension : {0, -1, 65537}) {
        write(primary + s::dimensions_offset, dimension);
        expect(!s::read_primary_clipmaps(primary, read), "uninitialized or unbounded grid rejected"); memory = paired;
    }
    for (const auto address : {primary + s::clipmap_count_offset, primary + s::dimensions_offset,
             primary + s::clipmaps_offset, primary + s::clipmaps_offset + 0x48}) {
        unreadable = address;
        expect(!s::read_primary_clipmaps(primary, read), "partial numeric reads cannot publish a buffer");
    }
    unreadable = 0;
    int reads{};
    auto changing = [&](uintptr_t at, void* out, size_t size) {
        const bool ok = read(at, out, size);
        if (at == primary + s::clipmap_count_offset && ++reads == 2) { *static_cast<int32_t*>(out) = 4; }
        return ok;
    };
    expect(!s::read_primary_clipmaps(primary, changing), "changed count invalidates the numeric snapshot");
    for (const auto address : {right + 0x10, right + 0x18, right + s::exposure_state_offset,
             right + s::view_info_state_offset, left + s::view_info_state_offset, family + 8, family + 0x28, primary}) {
        unreadable = address;
        expect(!s::missing_secondary_source(right, true, read, valid), "incomplete ownership evidence cannot borrow primary inputs");
    }
    unreadable = 0;
    for (const auto [address, value] : std::array{
             std::pair{right + s::exposure_state_offset, secondary},
             std::pair{right + s::view_info_state_offset, primary}, std::pair{right + 0x10, family + 8},
             std::pair{left + s::view_info_state_offset, secondary}, std::pair{primary, table + 8},
             std::pair{data + 8, left}}) {
        write(address, value);
        expect(!s::missing_secondary_source(right, true, read, valid), "stale, self-linked or mismatched eyes rejected"); memory = paired;
    }
    for (const auto [address, value] : std::array{
             std::pair{right + 0x13A0, int32_t{1}}, std::pair{right + 0x13A4, int32_t{0}},
             std::pair{right + 0x13A8, int32_t{1}}, std::pair{family + 0x10, int32_t{3}},
             std::pair{family + 0x14, int32_t{1}}, std::pair{secondary + s::clipmap_count_offset, int32_t{1}}}) {
        write(address, value);
        expect(!s::missing_secondary_source(right, true, read, valid), "only the missing secondary of the exact stereo topology is eligible"); memory = paired;
    }
    write(data, right); write(family + 0x10, int32_t{1}); write(right + 0x13A0, int32_t{1});
    const auto singleton = memory;
    expect(s::missing_secondary_source(right, true, read, valid) == primary, "validated right singleton retains its constructor-owned primary link");
    expect(!s::missing_secondary_source(right, false, read, valid), "singleton cannot borrow outside the Native Fix adapter");
    write(right + 0x13A0, int32_t{2});
    expect(!s::missing_secondary_source(right, true, read, valid), "unsupported secondary singleton passes through unchanged");
    memory = singleton;
    expect(s::missing_secondary_source(right, true, read, valid).has_value() && memory == singleton,
        "singleton lookup has no persistent mutation or resource cache to retire");

    for (const auto path : {L"DuneSandbox-Win64-Shipping.exe", L"C:/Xbox/DUNESANDBOX-WINGDK-SHIPPING.EXE"}) {
        expect(uevr::games::is_dune_ue521_relocatable_runtime(path, 0x50002, 0x10000), "Steam and GDK may attempt independent contract validation");
        expect(!uevr::games::is_dune_ue521_relocatable_runtime(path, 0x50003, 0x10000) &&
            !uevr::games::is_dune_ue521_relocatable_runtime(path, 0x50002, 0x20000), "unvalidated engine versions unchanged");
    }
    for (const auto path : {L"DuneSandbox-Win64-Shipping.exe.bak", L"NotDuneSandbox-WinGDK-Shipping.exe", L"OtherUE52.exe"}) {
        expect(!uevr::games::is_dune_ue521_relocatable_runtime(path, 0x50002, 0x10000), "other games and basename lookalikes unchanged");
    }
    std::array<uint8_t, 0x400> renderer{};
    std::copy(uevr::dune_frame::begin_family_code.begin(), uevr::dune_frame::begin_family_code.end(), renderer.begin() + 0x320);
    expect(uevr::dune_frame::valid_begin_family_callback(renderer), "GDK must independently prove the family argument and virtual slot");
    for (size_t i = 0; i < uevr::dune_frame::begin_family_code.size(); ++i) {
        auto changed = renderer; changed[0x320 + i] ^= 1;
        expect(!uevr::dune_frame::valid_begin_family_callback(changed), "changed GDK callback topology cannot inherit Steam mappings");
    }
    std::copy(uevr::dune_frame::begin_family_code.begin(), uevr::dune_frame::begin_family_code.end(), renderer.begin());
    expect(!uevr::dune_frame::valid_begin_family_callback(renderer), "ambiguous GDK callback proof rejected");
}
