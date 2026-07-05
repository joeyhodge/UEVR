#include <Windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <uevr/PreInjectionShaderRegistry.hpp>

using Microsoft::WRL::ComPtr;

int main() {
    const auto helper = LoadLibraryW(L"UEVRShaderRegistryBootstrap.dll");
    if (helper == nullptr) {
        std::cerr << "helper load failed: " << GetLastError() << '\n';
        return 1;
    }

    const auto get_api = reinterpret_cast<UEVRShaderRegistryGetApiFn>(
        GetProcAddress(helper, "UEVRShaderRegistry_GetApi"));
    if (get_api == nullptr || get_api(999) != nullptr) {
        std::cerr << "ABI rejection failed\n";
        return 2;
    }

    const auto api = get_api(UEVR_SHADER_REGISTRY_ABI_V1);
    if (api == nullptr) {
        std::cerr << "v1 API unavailable\n";
        return 3;
    }

    UEVRShaderRegistryStatusV1 status{};
    for (size_t i = 0; i < 200; ++i) {
        if (api->get_status(&status)) {
            break;
        }
        Sleep(10);
    }

    ComPtr<ID3D12Device> device{};
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::cerr << "D3D12CreateDevice failed\n";
        return 4;
    }

    const char shader_source[] = "[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {}";
    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};
    if (FAILED(D3DCompile(shader_source, sizeof(shader_source), nullptr, nullptr, nullptr, "main", "cs_5_0",
            0, 0, &shader, &errors))) {
        std::cerr << "D3DCompile failed\n";
        return 5;
    }

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> serialized_root{};
    if (FAILED(D3D12SerializeRootSignature(
            &root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors))) {
        std::cerr << "root signature serialization failed\n";
        return 6;
    }

    ComPtr<ID3D12RootSignature> root_signature{};
    if (FAILED(device->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(),
            IID_PPV_ARGS(&root_signature)))) {
        std::cerr << "root signature creation failed\n";
        return 7;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature.Get();
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};

    std::vector<std::thread> workers{};
    for (size_t i = 0; i < 8; ++i) {
        workers.emplace_back([&]() {
            ComPtr<ID3D12PipelineState> pipeline{};
            device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    api->get_status(&status);
    if (status.compute_pipelines != 8 || status.unique_shaders != 1 || status.retained_bytes != shader->GetBufferSize()) {
        std::cerr << "unexpected registry status: compute=" << status.compute_pipelines
                  << " unique=" << status.unique_shaders << " bytes=" << status.retained_bytes << '\n';
        return 8;
    }

    uint64_t enumerated{};
    api->enumerate_records([](const UEVRShaderRegistryRecordV1*, void* context) {
        ++*static_cast<uint64_t*>(context);
    }, &enumerated);
    if (enumerated != 8) {
        std::cerr << "enumeration mismatch\n";
        return 9;
    }

    std::cout << "shader registry bootstrap test passed\n";
    return 0;
}
