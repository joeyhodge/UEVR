#include <cstdint>

// Harmless image for loaded/disk/PDB identity tests; no UEVR or engine initializer.
extern "C" __declspec(dllexport) uint32_t uevr_support_capture_fixture() {
    return 0x55565231;
}
