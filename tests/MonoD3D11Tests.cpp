#include "mods/vr/MonoD3D11.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace mono = uevr::mono::dx11;
static int failures{};
static void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context))) { return 1; }

    mono::Retirement retirement;
    const auto wait_retired = [&](uint64_t token) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            if (retirement.poll(context.Get(), token)) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };

    for (const auto width : {16u, 64u, 32u}) {
        constexpr UINT height = 8;
        std::vector<uint32_t> pixels(width * height);
        for (UINT y = 0; y != height; ++y) {
            for (UINT x = 0; x != width; ++x) { pixels[y * width + x] = x < width / 2 ? (0xff000000 | (y << 8) | x) : 0xff00ff00; }
        }
        D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1,0}, D3D11_USAGE_DEFAULT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
        ComPtr<ID3D11Texture2D> source, destination, readback;
        expect(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &source)), "source allocation");
        expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &destination)), "destination allocation");
        expect(mono::copy_scene(context.Get(), source.Get(), destination.Get()), "validated Mono DX11 copy");
        source.Reset(); // Immediate-context commands must own their queued resource references.
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)), "readback allocation");
        context->CopyResource(readback.Get(), destination.Get());
        retirement.invalidate();
        expect(!retirement.poll(context.Get(), width), "first poll issues a query, never spins/waits");
        expect(wait_retired(width), "DX11 event completes queued copies before retirement");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            bool equal = true;
            for (UINT y = 0; y != height; ++y) {
                const auto row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
                for (UINT x = 0; x != width; ++x) { equal &= row[x] == pixels[y * width + (x % (width / 2))]; }
            }
            expect(equal, "both eyes contain exactly the current left source pixels, across resizes");
            context->Unmap(readback.Get(), 0);
        } else { expect(false, "readback map"); }

        expect(!mono::copy_scene(context.Get(), destination.Get(), destination.Get()), "self-copy rejected");
        expect(!mono::copy_scene(context.Get(), destination.Get(), readback.Get()), "staging output rejected");
        desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.Width = width / 2;
        ComPtr<ID3D11Texture2D> wrong_size;
        device->CreateTexture2D(&desc, nullptr, &wrong_size);
        expect(!mono::copy_scene(context.Get(), destination.Get(), wrong_size.Get()), "resize mismatch rejected");
        desc.Width = width; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D11Texture2D> wrong_format;
        device->CreateTexture2D(&desc, nullptr, &wrong_format);
        expect(!mono::copy_scene(context.Get(), destination.Get(), wrong_format.Get()), "RGBA/BGRA channel mismatch rejected");
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.ArraySize = 2;
        ComPtr<ID3D11Texture2D> array_texture;
        expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &array_texture)), "array allocation");
        expect(!mono::copy_scene(context.Get(), destination.Get(), array_texture.Get()), "array target rejected");
        desc.ArraySize = 1; desc.MipLevels = 2;
        ComPtr<ID3D11Texture2D> mip_texture;
        expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &mip_texture)), "mip allocation");
        expect(!mono::copy_scene(context.Get(), destination.Get(), mip_texture.Get()), "mip target rejected");
        desc.MipLevels = 1;
        ComPtr<ID3D11Device> other_device;
        ComPtr<ID3D11DeviceContext> other_context;
        expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &other_device, nullptr, &other_context)), "second device allocation");
        ComPtr<ID3D11Texture2D> foreign_texture;
        if (other_device) {
            expect(SUCCEEDED(other_device->CreateTexture2D(&desc, nullptr, &foreign_texture)), "foreign texture allocation");
            expect(!mono::copy_scene(context.Get(), destination.Get(), foreign_texture.Get()), "cross-device target rejected");
        }
        expect(!retirement.poll(context.Get(), width + 1), "new transition cannot reuse old event completion");
        expect(wait_retired(width + 1), "new transition can retire independently");
    }
    ComPtr<ID3D11DeviceContext> deferred;
    device->CreateDeferredContext(0, &deferred);
    expect(!retirement.poll(deferred.Get(), 1), "deferred contexts cannot poll retirement");
    std::printf("Mono DX11 WARP copy/lifetime checks: %d failures\n", failures);
    return failures != 0;
}
