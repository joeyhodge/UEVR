#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sdk/FinchGame.hpp>

namespace {
struct FinchFixture {
    struct Block { uintptr_t address; std::vector<uint8_t> bytes; bool code{}; };
    uintptr_t base{0x140000000};
    std::vector<Block> blocks;
    bool readable{true}, executable{true};
    static constexpr uintptr_t options = 0x300000000, family = 0x300001000;
    static constexpr uintptr_t state = 0x300002000, target = 0x300003000, table = 0x300004000;
    static constexpr uintptr_t wrapper = 0x300005000, rhi = 0x300006000, native = 0x300007000;
    static constexpr uintptr_t left_view = 0x300008000, right_view = 0x300009000;

    void add(uintptr_t address, size_t size, bool code = false) { blocks.push_back({address, std::vector<uint8_t>(size), code}); }
    template<class T> void put(uintptr_t address, T value) {
        for (auto& b : blocks) {
            if (address >= b.address && address - b.address <= b.bytes.size() && sizeof(T) <= b.bytes.size() - (address - b.address)) {
                std::memcpy(b.bytes.data() + address - b.address, &value, sizeof(T)); return;
            }
        }
        throw std::runtime_error("Finch fixture write outside a block");
    }
    sdk::discovery::Memory memory() {
        return {this, [](void* context, uintptr_t address, void* output, size_t size) {
            auto& f = *static_cast<FinchFixture*>(context);
            if (!f.readable) { return false; }
            for (const auto& b : f.blocks) {
                if (address >= b.address && address - b.address <= b.bytes.size() && size <= b.bytes.size() - (address - b.address)) {
                    std::memcpy(output, b.bytes.data() + address - b.address, size); return true;
                }
            }
            return false;
        }, [](void* context, uintptr_t address, size_t size) {
            auto& f = *static_cast<FinchFixture*>(context);
            if (!f.executable) { return false; }
            for (const auto& b : f.blocks) {
                if (b.code && address >= b.address && address - b.address <= b.bytes.size() && size <= b.bytes.size() - (address - b.address)) { return true; }
            }
            return false;
        }};
    }
    explicit FinchFixture(const nlohmann::json& fixture, uintptr_t load_base = 0x140000000) : base{load_base} {
        add(base, 0x400);
        put(base, uint16_t{0x5a4d}); put(base + 0x3c, uint32_t{0x80}); put(base + 0x80, uint32_t{0x4550});
        put(base + 0x84, uint16_t{0x8664}); put(base + 0x86, uint16_t{9});
        put(base + 0x88, fixture.at("timestamp").get<uint32_t>()); put(base + 0x98, uint16_t{0x20b});
        put(base + 0xd0, fixture.at("image_size").get<uint32_t>());
        for (const auto& b : fixture.at("blocks")) {
            const auto hex = b.at("hex").get<std::string>();
            add(base + b.at("rva").get<uint32_t>(), hex.size() / 2, true);
            for (size_t i = 0; i < hex.size(); i += 2) { blocks.back().bytes[i / 2] = uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)); }
        }
        add(base + sdk::finch::texture_vtable_rva, 0x200);
        put(base + sdk::finch::texture_vtable_rva + 0x1d0, base + sdk::finch::code::update_resource_rva);
        put(base + sdk::finch::texture_vtable_rva + 0x1d8, base + sdk::finch::code::create_resource_rva);
        add(base + sdk::finch::rhi_texture_vtable_rva, 0x40);
        put(base + sdk::finch::rhi_texture_vtable_rva + 0x30, base + sdk::finch::code::native_texture_getter_rva);
        add(base + sdk::finch::render_wrapper_vtable_rva, 0x40);
        put(base + sdk::finch::render_wrapper_vtable_rva + 0x10, base + sdk::finch::code::wrapper_ref_getter_rva);
        put(base + sdk::finch::render_wrapper_vtable_rva + 0x30, base + sdk::finch::code::wrapper_setter_rva);
        add(options, 0x200); add(family, 0x80); add(state, 8); add(target, 0x10); add(table, 0x40);
        add(wrapper, 0x80); add(rhi, 0x90);
        put(wrapper, base + sdk::finch::render_wrapper_vtable_rva); put(wrapper + 8, rhi);
        put(rhi, base + sdk::finch::rhi_texture_vtable_rva); put(rhi + 0x88, native);
        put(target + 8, wrapper);
        put(options + 0xb0, family); put(options + 0xb8, state);
        put(options + 0x100, uint32_t{1}); put(options + 0x104, 100.0f);
        put(family + 0x18, target); put(family + 0x28, state);
        put(target, table); put(state, table); put(table, base + sdk::finch::code::render_target_destructor_rva);
        put(table + 8, base + sdk::finch::code::render_texture_ref_getter_rva);
        put(table + 0x28, base + sdk::finch::code::render_wrapper_getter_rva);
    }
};

void test_finch_viewport_rects(const nlohmann::json& captured) {
    namespace f = sdk::finch;
    FinchFixture fixture{captured};
    fixture.add(FinchFixture::left_view, 0x430);
    fixture.add(FinchFixture::right_view, 0x430);
    constexpr uint32_t w = 2472, h = 2416;
    const f::ViewRects left{{0, 0, w, h}, {0, 0, w, h}, {0, 0, w, h}};
    const f::ViewRects right{{w, 0, 2 * w, h}, {w, 0, 2 * w, h}, {w, 0, 2 * w, h}};
    fixture.put(FinchFixture::left_view + 0x30, left);
    fixture.put(FinchFixture::right_view + 0x30, right);
    fixture.put(FinchFixture::right_view, FinchFixture::family);
    fixture.put(FinchFixture::right_view + 8, FinchFixture::state);
    fixture.put(FinchFixture::right_view + 0x420, uint32_t{1});

    // Independent model of Finch Draw's area-versus-bounding-rectangle test.
    const auto black_fills = [](const f::Rect& a, const f::Rect& b) {
        const auto area = [](const f::Rect& r) { return int64_t(r[2] - r[0]) * (r[3] - r[1]); };
        const int64_t bounding = int64_t(std::max(a[2], b[2]) - std::min(a[0], b[0])) *
            (std::max(a[3], b[3]) - std::min(a[1], b[1]));
        return area(a) + area(b) != bounding;
    };
    expect(black_fills(left.unscaled, left.unscaled), "Finch overlapping legacy Native Fix rectangles reproduce the post-render full-viewport black fill");
    expect(!black_fills(left.unscaled, right.unscaled), "Finch original packed rectangles pass the viewport coverage check");
    expect(f::viewport_rect_contract(fixture.memory(), fixture.base), "Finch constructor and post-render overlap/black-fill code validate together");

    struct Writer {
        FinchFixture& fixture;
        unsigned writes{}, fail_at{}, corrupt_at{}, partial_at{};
        void reset() { writes = fail_at = corrupt_at = partial_at = 0; }
        static bool write(void* context, uintptr_t view, const f::ViewRects& rects) {
            auto& self = *static_cast<Writer*>(context);
            ++self.writes;
            if (self.writes == self.fail_at) { return false; }
            auto value = rects;
            if (self.writes == self.corrupt_at) { ++value.unscaled[2]; }
            if (self.writes == self.partial_at) {
                self.fixture.put(view + 0x30, value.render);
                return false;
            }
            self.fixture.put(view + 0x30, value);
            return true;
        }
    } writer{fixture};
    const auto start = [&](f::RightRectOverride& transaction, const f::ViewRects& metadata = f::ViewRects{}) {
        const auto& r = metadata == f::ViewRects{} ? right : metadata;
        return transaction.initialize(fixture.memory(), FinchFixture::left_view, FinchFixture::right_view,
            left.unscaled, left.unconstrained, r.unscaled, r.unconstrained, w, h, &writer, Writer::write);
    };
    const auto read_right = [&]() {
        f::ViewRects r{}; expect(f::read_view_rects(fixture.memory(), FinchFixture::right_view, r), "Finch right rectangles remain readable"); return r;
    };
    {
        f::RightRectOverride transaction;
        expect(start(transaction), "Finch can enter a bounded zero-origin right singleton transaction");
        expect(read_right() == left, "Finch render, unscaled and unconstrained capture rectangles all fit one eye");
        expect(!start(transaction), "Finch cannot re-enter an active rectangle transaction");
        expect(transaction.restore() && read_right() == right, "Finch restores all packed rectangles before Draw resumes");
        expect(!black_fills(left.unscaled, read_right().unscaled), "Finch restored renderer output cannot trigger the overlap blackout");
        expect(transaction.restore(), "Finch rectangle restoration is idempotent");
    }
    try {
        f::RightRectOverride transaction;
        expect(start(transaction), "Finch rectangle exception test enters the transaction");
        throw std::runtime_error("simulated renderer exception");
    } catch (const std::runtime_error&) {}
    expect(read_right() == right, "Finch early/exception unwinding restores the original packed right eye");
    f::ViewRects observed_left{};
    expect(f::read_view_rects(fixture.memory(), FinchFixture::left_view, observed_left) && observed_left == left,
        "Finch rectangle isolation never modifies the primary eye");
    uintptr_t family{}, state{}; uint32_t pass{};
    expect(fixture.memory().load(FinchFixture::right_view, family) && family == FinchFixture::family &&
        fixture.memory().load(FinchFixture::right_view + 8, state) && state == FinchFixture::state &&
        fixture.memory().load(FinchFixture::right_view + 0x420, pass) && pass == 1,
        "Finch rectangle isolation preserves family, scene state and Same Pass identity");
    for (unsigned failure : {1u, 2u, 3u}) {
        writer.reset();
        if (failure == 1) { writer.fail_at = 1; }
        if (failure == 2) { writer.corrupt_at = 1; }
        if (failure == 3) { writer.partial_at = 1; }
        f::RightRectOverride transaction;
        expect(!start(transaction) && transaction.restore() && read_right() == right,
            "Finch rejected, corrupt or partially written capture rectangles roll back before fallback");
    }
    writer.reset(); writer.fail_at = 2;
    {
        f::RightRectOverride transaction;
        expect(start(transaction) && !transaction.restore(), "Finch reports failed restoration instead of publishing a successful frame");
    }
    expect(read_right() == right, "Finch scope cleanup retries a failed rectangle restoration");
    writer.reset();
    auto stale = right; --stale.unscaled[2];
    { f::RightRectOverride transaction; expect(!start(transaction, stale) && !writer.writes,
        "Finch stale metadata is rejected without writing any rectangle"); }
    fixture.readable = false;
    { f::RightRectOverride transaction; expect(!start(transaction) && !writer.writes,
        "Finch unreadable views are rejected without writes"); }
    fixture.readable = true;
    expect(!f::rebase_right_rects(left, left, w, h), "Finch rejects an already-overlapping pair instead of treating it as packed fallback");
    expect(!f::rebase_right_rects(left, right, 0, h) && !f::rebase_right_rects(left, right, 16385, h),
        "Finch does not rebase invalid or oversized capture dimensions");
    auto invalid = right; ++invalid.unconstrained[2];
    expect(!f::rebase_right_rects(left, invalid, w, h), "Finch rejects a capture extending beyond its packed eye");
    auto boxed_left = left, boxed_right = right;
    boxed_left.render = boxed_left.unscaled = {0, 100, w, h - 100};
    boxed_right.render = boxed_right.unscaled = {w, 100, 2 * w, h - 100};
    expect(f::rebase_right_rects(boxed_left, boxed_right, w, h) == boxed_left,
        "Finch preserves valid letterboxing rather than forcing a full eye rectangle");
    f::ViewRects inaccessible{};
    expect(!f::read_view_rects(fixture.memory(), 0, inaccessible) &&
        !f::read_view_rects(fixture.memory(), 0x7ffffffffff8, inaccessible), "Finch rejects null and overflowing view pointers");
}

void test_finch_native_fix_contracts() {
    namespace f = sdk::finch;
    std::ifstream stream{std::filesystem::path{__FILE__}.parent_path() / "fixtures/finch-20170621-native-contracts.json"};
    expect(stream.good(), "Finch original-code fixture is present");
    if (!stream.good()) { return; }
    const auto captured = nlohmann::json::parse(stream);
    test_finch_viewport_rects(captured);
    FinchFixture good{captured};
    constexpr uintptr_t owner = 0x30000a000;
    good.add(owner, f::texture_size);
    good.put(owner + 0xa8, uint32_t{2472}); good.put(owner + 0xac, uint32_t{2416});
    good.put(owner + 0xd0, uint8_t{2}); good.put(owner + 0xcc, uint32_t{3});
    expect(f::initialized_linear_capture(good.memory(), owner, 2472, 2416),
        "Finch creates linear BGRA8 storage with no TargetGamma override or generated mips");
    for (const auto flags : {0u, 2u, 5u, 7u}) {
        auto wrong_encoding = good; wrong_encoding.put(owner + 0xcc, flags);
        expect(!f::initialized_linear_capture(wrong_encoding.memory(), owner, 2472, 2416),
            "Finch rejects the previous extra-sRGB encoding and mip-generating captures");
    }
    for (const auto gamma : {2.2f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        auto override_gamma = good; override_gamma.put(owner + 0xa0, gamma);
        expect(!f::initialized_linear_capture(override_gamma.memory(), owner, 2472, 2416),
            "Finch rejects an owner TargetGamma that could override linear RHI creation");
    }
    auto wrong_format = good; wrong_format.put(owner + 0xd0, uint8_t{10});
    expect(!f::initialized_linear_capture(wrong_format.memory(), owner, 2472, 2416) &&
        !f::initialized_linear_capture(good.memory(), owner, 2473, 2416) &&
        !f::initialized_linear_capture(good.memory(), owner, 0, 2416) &&
        !f::initialized_linear_capture(good.memory(), 0x7ffffffffff8, 2472, 2416),
        "Finch linear capture validation also enforces format, exact dimensions and readable owner bounds");
    expect(f::matches_executable(L"D:\\Games\\EdithFinch\\FinchGame.exe") && f::matches_executable(L"FINCHGAME.EXE"), "Finch exact basename supports both path separators/case");
    expect(!f::matches_executable(L"FinchGameLauncher.exe") && !f::matches_executable(L"FinchGame.exe.bak") &&
        !f::matches_executable(L"DaysGone.exe"), "Finch repair does not match another executable");
    expect(f::constructor(good.memory(), good.base) == good.base + f::code::constructor_prefix_rva, "Finch original constructor copies validate without InstancedStereo");
    expect(f::target_factory(good.memory(), good.base), "Finch original initializer/resource virtuals validate");
    expect(f::wrapper_accessors(good.memory(), good.base), "Finch render wrapper is distinct from the real RHI texture");
    const auto reference = f::render_target_reference(good.memory(), good.base, FinchFixture::target);
    expect(reference.handled && reference.address == FinchFixture::wrapper + 8, "Finch follows the engine's double-hop getter, not cached raw-wrapper slot 5");
    expect(f::native_resource(good.memory(), good.base, FinchFixture::rhi) == FinchFixture::native &&
        f::native_resource(good.memory(), good.base, FinchFixture::wrapper) == FinchFixture::native,
        "Finch resolves real/wrapped textures without invoking the wrapper setter");
    const auto layout = f::view_layout(good.memory(), FinchFixture::options);
    expect(layout && layout->view_family == 0xb0 && layout->scene_state == 0xb8 && !layout->player_index &&
        layout->stereo_pass == 0x100 && layout->world_to_meters == 0x104, "Finch legacy pass/world offsets have no invented player index or StereoIPD");
    FinchFixture relocated{captured, 0x7ff600000000};
    expect(f::constructor(relocated.memory(), relocated.base).has_value() && f::target_factory(relocated.memory(), relocated.base), "Finch witnesses and vtables support ASLR");

    for (const auto& block : good.blocks) {
        if (!block.code) { continue; }
        for (size_t i = 0; i < block.bytes.size(); ++i) {
            auto changed = good;
            changed.put(block.address + i, uint8_t(block.bytes[i] ^ 1));
            const bool view = block.address < good.base + f::code::constructor_prefix_rva + 0x200 &&
                block.address >= good.base + f::code::constructor_prefix_rva;
            const bool factory = block.address == good.base + f::code::init_custom_format_rva ||
                block.address == good.base + f::code::update_resource_rva || block.address == good.base + f::code::create_resource_rva ||
                block.address == good.base + f::code::capture_gamma_getter_rva || block.address == good.base + f::code::capture_init_encoding_rva;
            const bool viewport = block.address == good.base + f::code::viewport_rect_area_rva ||
                block.address == good.base + f::code::viewport_overlap_rejection_rva ||
                block.address == good.base + f::code::viewport_black_fill_rva;
            expect(view ? !f::constructor(changed.memory(), changed.base) : factory ?
                !f::target_factory(changed.memory(), changed.base) : viewport ?
                !f::viewport_rect_contract(changed.memory(), changed.base) : !f::wrapper_accessors(changed.memory(), changed.base),
                "Finch rejects every changed code-witness byte");
        }
    }
    auto changed = good;
    changed.put(good.base + 0x88, f::timestamp + 1);
    expect(!f::constructor(changed.memory(), changed.base) && !f::target_factory(changed.memory(), changed.base), "Finch rejects another build timestamp");
    changed = good; changed.put(good.base + 0xd0, f::image_size + 0x1000);
    expect(!f::target_factory(changed.memory(), changed.base), "Finch rejects another image size");
    changed = good; changed.put(good.base + 0x86, uint16_t{10});
    expect(f::target_factory(changed.memory(), changed.base), "Finch accepts the original and post-startup section counts");
    changed.put(good.base + 0x86, uint16_t{11});
    expect(!f::target_factory(changed.memory(), changed.base), "Finch rejects unexpected section counts");
    changed = good; changed.put(good.base + f::texture_vtable_rva + 0x1d0, good.base + f::code::create_resource_rva);
    expect(!f::target_factory(changed.memory(), changed.base), "Finch rejects swapped resource virtuals instead of probing them");
    changed = good; changed.readable = false;
    expect(!f::constructor(changed.memory(), changed.base) && !f::target_factory(changed.memory(), changed.base), "Finch rejects inaccessible code");
    changed = good; changed.executable = false;
    expect(!f::constructor(changed.memory(), changed.base) && !f::target_factory(changed.memory(), changed.base), "Finch requires executable image ownership");
    changed = good; changed.put(FinchFixture::options + 0x100, uint32_t{3});
    expect(!f::view_layout(changed.memory(), FinchFixture::options), "Finch rejects an invalid stereo pass");
    changed = good; changed.put(FinchFixture::options + 0x104, std::numeric_limits<float>::quiet_NaN());
    expect(!f::view_layout(changed.memory(), FinchFixture::options), "Finch rejects a nonfinite world scale");
    changed = good; changed.put(FinchFixture::options + 0xb8, uintptr_t{0});
    expect(!f::view_layout(changed.memory(), FinchFixture::options), "Finch waits for a usable scene state");
    expect(!f::view_layout(good.memory(), 0) && !f::view_layout(good.memory(), 0x7ffffffffff8), "Finch rejects null/overflowing options");
    expect(f::dimensions(1, 1) && f::dimensions(16384, 16384) && !f::dimensions(0, 1) && !f::dimensions(16385, 1), "Finch target dimensions are bounded");
    changed = good; changed.put(FinchFixture::wrapper + 8, uintptr_t{0});
    const auto pending = f::render_target_reference(changed.memory(), changed.base, FinchFixture::target);
    expect(pending.handled && !pending.address && !f::native_resource(changed.memory(), changed.base, FinchFixture::wrapper),
        "Finch pending wrappers wait without falling back to an unsafe cached slot");
    changed = good; changed.put(FinchFixture::table + 8, good.base + f::code::wrapper_setter_rva);
    const auto wrong_slot = f::render_target_reference(changed.memory(), changed.base, FinchFixture::target);
    expect(wrong_slot.handled && !wrong_slot.address, "Finch recognizes its capture but rejects a setter in place of the texture getter");
    changed = good; changed.put(FinchFixture::rhi, good.base + f::render_wrapper_vtable_rva);
    expect(!f::native_resource(changed.memory(), changed.base, FinchFixture::wrapper), "Finch rejects recursive/mistyped wrappers");
}

void test_finch_native_fix_image(const char* path) {
    std::ifstream stream{path, std::ios::binary};
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    if (bytes.size() != sdk::finch::image_size) { expect(false, "Finch image has the complete RVA-mapped capture extent"); return; }
    uint32_t nt{}; uintptr_t base{};
    std::memcpy(&nt, bytes.data() + 0x3c, sizeof(nt));
    if (nt > 0x300) { expect(false, "Finch image header is in bounds"); return; }
    std::memcpy(&base, bytes.data() + nt + 24 + 24, sizeof(base));
    struct Image { uintptr_t base; const std::vector<uint8_t>& bytes; } image{base, bytes};
    sdk::discovery::Memory memory{&image, [](void* c, uintptr_t p, void* out, size_t n) {
        const auto& i = *static_cast<Image*>(c);
        if (p < i.base || p - i.base > i.bytes.size() || n > i.bytes.size() - (p - i.base)) { return false; }
        std::memcpy(out, i.bytes.data() + p - i.base, n); return true;
    }, [](void* c, uintptr_t p, size_t n) {
        const auto& i = *static_cast<Image*>(c);
        return p >= i.base + 0x1000 && p - i.base < 0x1f1be6b && n <= 0x1f1be6b - (p - i.base);
    }};
    expect(sdk::finch::constructor(memory, base).has_value(), "Finch real captured constructor validates");
    expect(sdk::finch::target_factory(memory, base), "Finch real captured target ABI validates");
    expect(sdk::finch::wrapper_accessors(memory, base), "Finch real wrapper and native getter contracts validate");
    expect(sdk::finch::viewport_rect_contract(memory, base), "Finch real viewport overlap/black-fill path and rectangle member copies validate");
}
}
