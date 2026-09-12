#pragma once

#include <cstdint>
#include <optional>

namespace uevr::hifi {
// Read-only discovery; it never enables the protection backend.
std::optional<uintptr_t> find_version_marker(uintptr_t image_base);
// Called before game-code hooks; only the exact game and validated UE4.27 evidence opt in.
bool initialize_hook_memory(bool (*is_ue427)());
}
