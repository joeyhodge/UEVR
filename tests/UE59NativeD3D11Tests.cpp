#include "mods/vr/UE59NativeD3D11.hpp"

#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
static int failures{};
static void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

static void test_native_policy() {
    namespace dx11 = uevr::ue59_native::dx11;
    expect(dx11::admits_linked_view_proof(true, false, false) &&
        dx11::admits_linked_view_proof(false, true, true) &&
        !dx11::admits_linked_view_proof(false, false, true) &&
        !dx11::admits_linked_view_proof(false, true, false),
        "only reviewed 5.9 DX11 joins the existing modern DX12 linked renderer");
    expect(dx11::mode_supported(true, true) &&
        !dx11::mode_supported(false, true) && !dx11::mode_supported(true, false),
        "5.9 DX11 linked rendering requires a standard OpenXR output");
    const auto clocks = uevr::native_frame::pack_clocks(90, 45);
    expect(dx11::split_pending(clocks, 90, 45) && dx11::split_pending(clocks, 90, 47) &&
        dx11::split_pending(clocks, 91, 46) && !dx11::split_pending(clocks, 92, 45) &&
        !dx11::split_pending(clocks, 91, 47) && !dx11::split_pending(UINT64_MAX, 90, 45),
        "a rejected split suppresses only the current draw and its one-frame handoff");
    const dx11::Binding binding{{3, 1, 2, 3, 4, 5, 6}};
    expect(binding.matches_capture(3, 1, 2, 4) &&
        !binding.matches_capture(4, 1, 2, 4) &&
        !binding.matches_capture(3, 7, 2, 4) &&
        !binding.matches_capture(3, 1, 8, 4) &&
        !binding.matches_capture(3, 1, 2, 9) &&
        !dx11::Binding{}.matches_capture(0, 0, 0, 0),
        "5.9 DX11 producer rejects stale generation, viewport, packed RHI and capture identities");
    auto replacement = binding.identity;
    ++replacement.swapchain;
    expect(replacement != binding.identity, "swapchain replacement needs a new preflight");
    auto packed = D3D11_TEXTURE2D_DESC{64, 16, 1, 1, DXGI_FORMAT_B8G8R8A8_TYPELESS, {1, 0},
        D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    auto right = packed;
    right.Width /= 2;
    expect(dx11::source_pair_matches(packed, right, 32, 16) &&
        !dx11::source_pair_matches(packed, right, 0, 16) &&
        !dx11::source_pair_matches(packed, right, UINT32_MAX, 16) &&
        !dx11::source_pair_matches(packed, right, 32, 17),
        "5.9 DX11 accepts exact double-wide/eye extents without overflow or scale guesses");
    right.SampleDesc.Count = 4;
    expect(!dx11::source_pair_matches(packed, right, 32, 16), "5.9 DX11 rejects MSAA capture sources");
    packed.ArraySize = 2;
    expect(!dx11::simple_bgra(packed), "5.9 DX11 does not treat an array as packed stereo");
}

static void test_native_pair(ID3D11Device* device, ID3D11DeviceContext* context) {
    namespace native = uevr::ue59_native::dx11;
    constexpr uint32_t eye_width = 32, height = 8;
    constexpr uint32_t packed_width = 2 * eye_width;
    constexpr uint32_t poisoned_right = 0xff0000ff, untouched = 0xffff00ff;
    std::vector<uint32_t> scene(packed_width * height, poisoned_right);
    std::vector<uint32_t> capture(eye_width * height);
    std::vector<uint32_t> sentinel(scene.size(), untouched);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < eye_width; ++x) {
            scene[y * packed_width + x] = 0xff008000 | (y << 8) | x;
            capture[y * eye_width + x] = 0xffc00000 | (y << 8) | x;
        }
    }
    const D3D11_TEXTURE2D_DESC scene_desc{packed_width, height, 1, 1, DXGI_FORMAT_B8G8R8A8_TYPELESS, {1, 0},
        D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    auto right_desc = scene_desc;
    right_desc.Width = eye_width;
    right_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    auto output_desc = scene_desc;
    output_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    auto staging_desc = output_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    const D3D11_SUBRESOURCE_DATA packed_data{scene.data(), packed_width * 4, 0};
    const D3D11_SUBRESOURCE_DATA right_data{capture.data(), eye_width * 4, 0};
    const D3D11_SUBRESOURCE_DATA output_data{sentinel.data(), packed_width * 4, 0};
    ComPtr<ID3D11Texture2D> packed, right, output, staging;
    const bool allocated = SUCCEEDED(device->CreateTexture2D(&scene_desc, &packed_data, &packed)) &&
        SUCCEEDED(device->CreateTexture2D(&right_desc, &right_data, &right)) &&
        SUCCEEDED(device->CreateTexture2D(&output_desc, &output_data, &output)) &&
        SUCCEEDED(device->CreateTexture2D(&staging_desc, nullptr, &staging));
    expect(allocated, "5.9 DX11 native pair BGRA allocations");
    if (!allocated) { return; }

    const auto check_pixels = [&](bool copied) {
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) { return false; }
        bool equal = true;
        for (uint32_t y = 0; y < height; ++y) {
            const auto row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
            for (uint32_t x = 0; x < packed_width; ++x) {
                const auto expected = !copied ? untouched : (x < eye_width
                    ? scene[y * packed_width + x] : capture[y * eye_width + x - eye_width]);
                equal &= row[x] == expected;
            }
        }
        context->Unmap(staging.Get(), 0);
        return equal;
    };

    const auto copy = [&](ID3D11DeviceContext* ctx, ID3D11Texture2D* second, ID3D11Texture2D* destination,
        uint32_t width) {
        return native::copy_pair(ctx, packed.Get(), second, destination, width, height);
    };
    expect(!copy(context, packed.Get(), output.Get(), eye_width), "aliased eye sources rejected");
    expect(!copy(context, right.Get(), packed.Get(), eye_width), "aliased destination rejected");
    expect(!copy(context, right.Get(), output.Get(), eye_width / 2), "stale eye extent rejected");
    expect(!copy(context, right.Get(), staging.Get(), eye_width), "staging destination rejected");
    ComPtr<ID3D11DeviceContext> deferred;
    expect(SUCCEEDED(device->CreateDeferredContext(0, &deferred)) &&
        !copy(deferred.Get(), right.Get(), output.Get(), eye_width), "deferred-context pair rejected");
    ComPtr<ID3D11Device> foreign_device;
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &foreign_device, nullptr, nullptr)), "foreign DX11 device allocated");
    ComPtr<ID3D11Texture2D> foreign_right;
    expect(foreign_device && SUCCEEDED(foreign_device->CreateTexture2D(&right_desc, &right_data, &foreign_right)) &&
        !copy(context, foreign_right.Get(), output.Get(), eye_width), "foreign right capture rejected");
    expect(check_pixels(false), "rejected second eye leaves both destination halves untouched");
    expect(copy(context, right.Get(), output.Get(), eye_width), "exact packed left and independent right copied");
    packed.Reset();
    right.Reset();
    expect(check_pixels(true), "HMD image contains distinct left/right pixels, not the stale packed right half");
}

int main() {
    test_native_policy();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context))) { return 1; }
    test_native_pair(device.Get(), context.Get());
    std::printf("UE5.9 Native Fix DX11 WARP checks: %d failures\n", failures);
    return failures != 0;
}
