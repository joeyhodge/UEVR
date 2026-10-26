#pragma once

#include <d3d12.h>
#include <windows.h>

namespace utility::d3d12_resource {
// The caller first establishes a live COM object from a trusted native module.
// A device/heap/queue can have the same module owner as a resource but a different
// GetDesc ABI. Query the interface before using any resource-only virtual.
inline bool query(IUnknown* object, ID3D12Resource** resource) {
    if (!resource) {
        return false;
    }
    *resource = nullptr;
    __try {
        return object && SUCCEEDED(object->QueryInterface(__uuidof(ID3D12Resource), reinterpret_cast<void**>(resource))) && *resource;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *resource = nullptr;
        return false;
    }
}
} // namespace utility::d3d12_resource
