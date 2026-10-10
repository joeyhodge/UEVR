#include "mods/vr/DuneNativeTransaction.hpp"

void test_dune_native_transaction() {
    namespace d = uevr::dune_native;
    std::vector<uint8_t> code(0x1600, 0x90);
    const auto put = [&](size_t offset, const auto& bytes) { std::copy(bytes.begin(), bytes.end(), code.begin() + offset); };
    put(0x49, d::owner_code); put(0x3B8, d::pass_code); put(0x496, d::index_code);
    put(0x1003, d::instanced_code); put(0x1057, d::multiview_code); put(0x116F, d::primary_code);
    expect(d::valid_constructor(code), "Dune independent constructor field proof is accepted");
    for (auto offset : {0x49, 0x3B8, 0x496, 0x1003, 0x1057, 0x116F}) {
        auto changed = code; changed[offset] ^= 1;
        expect(!d::valid_constructor(changed), "every independent Dune view-field proof is mandatory");
    }
    expect(!d::valid_constructor(std::span{code}.first(0x117A)), "truncated primary-index proof is rejected");
    auto duplicate = code;
    std::copy(d::pass_code.begin(), d::pass_code.end(), duplicate.begin() + 0x1300);
    expect(!d::valid_constructor(duplicate), "ambiguous constructor proof is rejected");
    code.resize(d::maximum_constructor_bytes + 1);
    expect(!d::valid_constructor(code), "constructor proof cannot escape its bounded function body");

    constexpr uintptr_t heap = 0x100000, family = heap + 0x100, data = heap + 0x200;
    constexpr uintptr_t left = heap + 0x1000, right = heap + 0x6000;
    constexpr uintptr_t left_state = heap + 0xA000, right_state = heap + 0xB000;
    std::vector<uint8_t> memory(0x10000);
    const auto write_value = [&](uintptr_t address, auto value) { std::memcpy(memory.data() + address - heap, &value, sizeof(value)); };
    write_value(family + 8, data); write_value(family + 0x10, int32_t{2}); write_value(family + 0x14, int32_t{2});
    write_value(data, left); write_value(data + 8, right);
    for (const auto view : {left, right}) {
        write_value(view + d::family_offset, family);
        write_value(view + d::state_offset, view == left ? left_state : right_state);
        write_value(view + d::pass_offset, view == left ? uint32_t{1} : uint32_t{2});
        write_value(view + d::pass_offset + 4, view == left ? int32_t{0} : int32_t{1});
        write_value(view + d::pass_offset + 8, int32_t{0});
        write_value(view + 0x2478, left_state); // Authored shared eye adaptation must stay untouched.
    }
    const auto pair = memory;
    int writes{};
    bool writable = true, corrupt_write = false;
    uintptr_t unreadable{};
    const auto in_range = [&](uintptr_t address, size_t size) {
        return address >= heap && address - heap <= memory.size() && size <= memory.size() - (address - heap);
    };
    auto read = [&](uintptr_t address, void* value, size_t size) {
        if (!in_range(address, size) || (unreadable >= address && unreadable - address < size)) { return false; }
        std::memcpy(value, memory.data() + address - heap, size); return true;
    };
    auto write = [&](uintptr_t address, const void* value, size_t size) {
        if (!writable || !in_range(address, size)) { return false; }
        expect(address == right + d::pass_offset && size == 4, "adapter writes ONLY the four-byte right StereoPass");
        ++writes; std::memcpy(memory.data() + address - heap, value, size);
        if (corrupt_write) { memory[address - heap] = 3; corrupt_write = false; }
        return true;
    };
    const auto prepare = [&](auto& transaction, bool validated = true, uint64_t generation = 7) {
        return transaction.prepare(validated, family, left, right, left_state, right_state, generation);
    };
    const auto singleton = [&] { write_value(data, right); write_value(family + 0x10, int32_t{1}); };
    const auto right_pass = [&] { return d::read_eye(right, read)->pass; };

    {
        d::SingletonPrimary transaction{read, write};
        expect(prepare(transaction) && memory == pair && writes == 0, "preparing a proven pair does not relabel either constructor");
        expect(!transaction.apply(7) && memory == pair, "warmup and paired transition fallback cannot become two PRIMARY eyes");
        singleton(); const auto before = memory;
        expect(!transaction.apply(8) && memory == before, "retired capture generation cannot mutate the right eye");
        expect(transaction.apply(7) && right_pass() == 1, "right singleton alone is adapted while its render copy executes");
        auto expected = before; expected[right - heap + d::pass_offset] = 1;
        expect(memory == expected, "state, exposure, projection, stereo index and family storage are not changed by the adapter");
        expect(!transaction.apply(7), "active adapter cannot be applied recursively");
        expect(transaction.restore() && memory == before && transaction.restore(), "restoration is exact and idempotent");
    }
    memory = pair;
    for (bool exception : {false, true}) {
        const auto exercise = [&] {
            d::SingletonPrimary transaction{read, write};
            expect(prepare(transaction), "scoped render preparation succeeds");
            singleton();
            expect(transaction.apply(7), "scoped render application succeeds");
            if (exception) { throw 1; }
        };
        try { exercise(); } catch (int) {}
        expect(right_pass() == 2, "normal return and exception unwind restore authored SECONDARY identity");
        memory = pair;
    }

    {
        d::SingletonPrimary transaction{read, write};
        const auto before_writes = writes;
        expect(!prepare(transaction, false) && !prepare(transaction, true, 0), "unproven layouts and unpublished generations stay unchanged");
        expect(!transaction.apply(7) && memory == pair && writes == before_writes, "failed preparation cannot arm a later write");
        expect(prepare(transaction) && !prepare(transaction, false), "a rejected re-prepare clears the previous prepared token");
        singleton(); expect(!transaction.apply(7), "previous successful preparation cannot survive a rejected replacement");
    }
    memory = pair;
    for (const auto offset : {d::family_offset, d::state_offset, d::pass_offset, d::stereo_flags_offset}) {
        d::SingletonPrimary transaction{read, write};
        unreadable = right + offset;
        const auto before_writes = writes;
        expect(!prepare(transaction) && writes == before_writes, "unreadable metadata rejects before any mutation");
        unreadable = 0;
    }
    const auto reject_field = [&](uintptr_t address, auto value) {
        memory = pair; write_value(address, value); const auto before = memory;
        d::SingletonPrimary transaction{read, write};
        expect(!prepare(transaction) && memory == before, "changed topology or eye identity rejects without mutating fallback");
    };
    reject_field(right + d::family_offset, family + 8);
    reject_field(right + d::state_offset, left_state);
    reject_field(right + d::pass_offset, uint32_t{1});
    reject_field(right + d::pass_offset + 4, int32_t{0});
    reject_field(right + d::pass_offset + 8, int32_t{1});
    reject_field(right + d::stereo_flags_offset, uint8_t{1});
    reject_field(right + d::stereo_flags_offset + 3, uint8_t{1});
    reject_field(family + 0x10, int32_t{1});
    reject_field(family + 0x14, int32_t{1});
    reject_field(data + 8, left);
    memory = pair;
    {
        d::SingletonPrimary transaction{read, write};
        expect(prepare(transaction), "write-failure test prepares"); singleton();
        const auto before = memory; writable = false;
        expect(!transaction.apply(7) && memory == before, "read-only view metadata cannot produce a successful transaction");
        writable = true; expect(transaction.restore() && right_pass() == 2, "failed application retains a restoration obligation");
    }
    memory = pair;
    {
        d::SingletonPrimary transaction{read, write};
        expect(prepare(transaction), "readback-failure test prepares"); singleton(); const auto before = memory;
        corrupt_write = true;
        expect(!transaction.apply(7) && memory == before, "incorrect applied pass is rejected and immediately restored");
    }
    memory = pair;
    {
        d::SingletonPrimary transaction{read, write};
        expect(prepare(transaction), "post-primary identity test prepares"); singleton();
        write_value(right + d::state_offset, right_state + 8); const auto before = memory;
        expect(!transaction.apply(7) && memory == before, "state change between renderer calls cannot use a prepared token");
    }
}
