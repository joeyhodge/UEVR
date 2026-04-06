#pragma once

#include <cstdint>

namespace vrmod {
struct UE57SlateSymbols {
    uintptr_t add_slate_draw_elements_pass{0};
    uintptr_t register_external_texture_from_rhi{0};

    bool valid() const {
        return add_slate_draw_elements_pass != 0 && register_external_texture_from_rhi != 0;
    }
};

const UE57SlateSymbols& get_ue57_slate_symbols();
}
