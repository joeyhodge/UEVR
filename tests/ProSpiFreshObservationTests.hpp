#pragma once

#include "mods/vr/ProSpiFreshObservation.hpp"
#include <array>
#include <cstring>

namespace prospi_fresh_observation_tests {
struct SnapshotMemory {
    static constexpr uintptr_t base = 0x10000, array = base + 0x100;
    static constexpr uintptr_t items = base + 0x400, object = base + 0x800;
    static constexpr uintptr_t item = items + 0x18;
    std::array<uint8_t, 0x1000> bytes{};
    size_t item_reads{}, object_reads{}, reads{};
    unsigned change{};
    uintptr_t denied{};
    template<class T> void put(uintptr_t address, T value) {
        std::memcpy(bytes.data() + address - base, &value, sizeof(value));
    }
    SnapshotMemory() {
        put<uintptr_t>(array + 0x10, items);
        put<int32_t>(array + 0x18, 2); put<int32_t>(array + 0x1C, 2);
        put<uintptr_t>(item, object); put<int32_t>(item + 0x10, 19);
        put<uintptr_t>(object, base + 0xC00); put<int32_t>(object + 12, 1);
    }
    static bool read(void* context, uintptr_t address, void* output, size_t size) {
        auto& self = *static_cast<SnapshotMemory*>(context);
        ++self.reads;
        if (address < base || address - base > self.bytes.size() ||
            size > self.bytes.size() - (address - base) || address == self.denied) { return false; }
        if (address == item && ++self.item_reads == 2) {
            if (self.change == 1) { self.put<int32_t>(item + 0x10, 20); }
            if (self.change == 2) { self.put<uintptr_t>(item, object + 0x80); }
            if (self.change == 5) { self.put<uint32_t>(item + 8, 1u << 29); }
        }
        if (address == object && ++self.object_reads == 2) {
            if (self.change == 3) { self.put<int32_t>(object + 12, 0); }
            if (self.change == 4) { self.put<uintptr_t>(object, base + 0xC08); }
        }
        std::memcpy(output, self.bytes.data() + address - base, size);
        return true;
    }
};

template<class Expect> void run(const Expect& expect) {
    using namespace sdk::object_liveness;
    sdk::object_array::Layout layout{}, other_layout{};
    int header{}, other_header{};
    std::optional<Observation> value = Observation{{0x1000, 0x3000, 7, 19, &layout, &header}, 0};
    const auto read = [&](Policy policy = Policy::legacy_unchanged) {
        return uevr::prospi::observation::fresh(value, 0x1000, &layout, &header, policy);
    };
    expect(read() && matches(*read(), value->identity), "fresh SDK observation retains the complete index/serial/table identity");
    for (const int32_t serial : {-1, 0, 23}) {
        value->identity.serial = serial;
        expect(read() && read()->serial == serial, "missing, unassigned and assigned SDK serials retain their exact meaning");
    }
    for (uint32_t bit{}; bit < 32; ++bit) {
        value->flags = 1u << bit;
        for (const auto policy : {Policy::legacy_unchanged, Policy::ue54_garbage, Policy::ue55_to_58_garbage_unreachable}) {
            expect(read(policy).has_value() == !rejected(policy, value->flags),
                "every individual item flag retains the SDK policy, including live reachability/refcount bits");
        }
    }
    value->flags = 1u << 29;
    expect(!read(), "legacy unreachable observation is rejected without re-observing an already double-witnessed item");
    value->flags = 1u << 21;
    expect(!read(Policy::ue54_garbage) && !read(Policy::ue55_to_58_garbage_unreachable),
        "current SDK garbage policy remains authoritative");
    value->flags = 1u << 28;
    expect(!read(Policy::ue55_to_58_garbage_unreachable), "newer unreachable bits are not ignored");
    value->flags = 0;
    value->identity.layout = &other_layout;
    expect(!read(), "array-layout republication invalidates a fresh observation");
    value->identity.layout = &layout; value->identity.header_layout = &other_header;
    expect(!read(), "object-header republication cannot be mistaken for the same observation contract");
    value->identity.header_layout = &header;
    expect(!uevr::prospi::observation::fresh(value, 0x1000, nullptr, &header, Policy::legacy_unchanged) &&
        !uevr::prospi::observation::fresh(value, 0x1000, &layout, nullptr, Policy::legacy_unchanged),
        "an unpublished header or array contract is not accepted");
    expect(!uevr::prospi::observation::fresh(value, 0x2000, &layout, &header, Policy::legacy_unchanged),
        "a different object cannot borrow a fresh observation");
    value->identity.index = -1;
    expect(!read(), "invalid object-array indices are rejected");
    value->identity.index = 7; value->identity.vtable = 0;
    expect(!read(), "missing vtables are rejected");
    value.reset();
    expect(!read(), "failed SDK witnesses stay failed");

    for (unsigned change{}; change <= 5; ++change) {
        SnapshotMemory memory;
        memory.change = change;
        const sdk::discovery::Memory reader{&memory, SnapshotMemory::read, nullptr};
        const auto observed = observe_identity(reader, SnapshotMemory::array, layout,
            SnapshotMemory::object, 12, false, &header);
        const auto fresh = uevr::prospi::observation::fresh(observed, SnapshotMemory::object,
            &layout, &header, Policy::legacy_unchanged);
        expect(fresh.has_value() == (change == 0),
            "actual SDK double witnesses reject serial, item, index, table and liveness changes");
        if (!change) {
            expect(memory.item_reads == 2 && memory.object_reads == 2 && memory.reads == 5,
                "fresh consumption keeps both SDK witnesses without a redundant second observation");
        }
    }
    SnapshotMemory denied;
    denied.denied = SnapshotMemory::item;
    expect(!observe_identity({&denied, SnapshotMemory::read, nullptr}, SnapshotMemory::array, layout,
        SnapshotMemory::object, 12, false, &header), "unreadable item memory is rejected by the original SDK reader");
}
}
