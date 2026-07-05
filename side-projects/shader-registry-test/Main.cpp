#include <Windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include <uevr/PreInjectionShaderRegistry.hpp>

using Microsoft::WRL::ComPtr;

using CreateComputePipelineStateFn = HRESULT (WINAPI*)(
    ID3D12Device*,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC*,
    REFIID,
    void**);

CreateComputePipelineStateFn g_chained_compute_original{};
std::atomic<uint32_t> g_chained_compute_calls{};

struct EnumeratedRecords {
    uintptr_t library_graphics{};
    uintptr_t library_compute{};
    uintptr_t library_stream{};
    uintptr_t library_graphics_stream{};
    bool saw_library_graphics{};
    bool saw_library_compute{};
    bool saw_library_stream{};
    bool saw_library_graphics_stream{};
    uint64_t total{};
};

void collect_record(const UEVRShaderRegistryRecordV1* record, void* context) {
    if (record == nullptr || context == nullptr) {
        return;
    }

    auto& records = *static_cast<EnumeratedRecords*>(context);
    ++records.total;
    if (record->pipeline_state == records.library_graphics) {
        records.saw_library_graphics =
            record->kind == UEVRShaderRegistryRecord_Graphics &&
            record->vertex_hash[0] != '\0' &&
            record->pixel_hash[0] != '\0';
    } else if (record->pipeline_state == records.library_compute) {
        records.saw_library_compute =
            record->kind == UEVRShaderRegistryRecord_Compute &&
            record->compute_hash[0] != '\0';
    } else if (record->pipeline_state == records.library_stream) {
        records.saw_library_stream =
            record->kind == UEVRShaderRegistryRecord_PipelineStream &&
            record->compute_hash[0] != '\0' &&
            record->descriptor_hash[0] != '\0';
    } else if (record->pipeline_state == records.library_graphics_stream) {
        records.saw_library_graphics_stream =
            record->kind == UEVRShaderRegistryRecord_PipelineStream &&
            record->vertex_hash[0] != '\0' &&
            record->pixel_hash[0] != '\0' &&
            record->descriptor_hash[0] != '\0';
    }
}

template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) StreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type};
    T payload{};
};

struct alignas(void*) ComputePipelineStream {
    StreamSubobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature{};
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS> compute_shader{};
};

struct alignas(void*) GraphicsPipelineStream {
    StreamSubobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature{};
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vertex_shader{};
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> pixel_shader{};
    StreamSubobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend{};
    StreamSubobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sample_mask{};
    StreamSubobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer{};
    StreamSubobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth_stencil{};
    StreamSubobject<D3D12_INPUT_LAYOUT_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT> input_layout{};
    StreamSubobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology{};
    StreamSubobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> render_targets{};
    StreamSubobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample_desc{};
};

HRESULT WINAPI chained_create_compute_pipeline_state(
    ID3D12Device* device,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** pipeline_state) {
    g_chained_compute_calls.fetch_add(1, std::memory_order_relaxed);
    return g_chained_compute_original(device, desc, riid, pipeline_state);
}

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
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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

    ComPtr<ID3D12Device1> device1{};
    ComPtr<ID3D12Device2> device2{};
    if (FAILED(device.As(&device1)) || FAILED(device.As(&device2))) {
        std::cerr << "D3D12 device does not expose pipeline-library interfaces\n";
        return 9;
    }

    ComPtr<ID3D12PipelineLibrary1> pipeline_library{};
    if (FAILED(device1->CreatePipelineLibrary(
            nullptr, 0, IID_PPV_ARGS(&pipeline_library)))) {
        std::cerr << "CreatePipelineLibrary failed\n";
        return 10;
    }

    ComPtr<ID3D12PipelineState> stored_compute{};
    if (FAILED(device->CreateComputePipelineState(
            &desc, IID_PPV_ARGS(&stored_compute))) ||
        FAILED(pipeline_library->StorePipeline(L"compute", stored_compute.Get()))) {
        std::cerr << "could not seed compute pipeline library entry\n";
        return 11;
    }

    ComPtr<ID3D12PipelineState> loaded_compute{};
    if (FAILED(pipeline_library->LoadComputePipeline(
            L"compute", &desc, IID_PPV_ARGS(&loaded_compute)))) {
        std::cerr << "LoadComputePipeline failed\n";
        return 12;
    }

    const char vertex_source[] =
        "float4 main(float4 position : POSITION) : SV_Position { return position; }";
    const char pixel_source[] =
        "float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }";
    ComPtr<ID3DBlob> vertex_shader{};
    ComPtr<ID3DBlob> pixel_shader{};
    if (FAILED(D3DCompile(
            vertex_source, sizeof(vertex_source), nullptr, nullptr, nullptr, "main", "vs_5_0",
            0, 0, &vertex_shader, &errors)) ||
        FAILED(D3DCompile(
            pixel_source, sizeof(pixel_source), nullptr, nullptr, nullptr, "main", "ps_5_0",
            0, 0, &pixel_shader, &errors))) {
        std::cerr << "graphics shader compilation failed\n";
        return 13;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc{};
    graphics_desc.pRootSignature = root_signature.Get();
    graphics_desc.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
    graphics_desc.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
    auto& blend = graphics_desc.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics_desc.SampleMask = UINT_MAX;
    graphics_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    graphics_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    graphics_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    graphics_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    graphics_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    graphics_desc.RasterizerState.DepthClipEnable = TRUE;
    graphics_desc.DepthStencilState.DepthEnable = FALSE;
    graphics_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    graphics_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    graphics_desc.DepthStencilState.StencilEnable = FALSE;
    graphics_desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    graphics_desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    graphics_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    graphics_desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    graphics_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    graphics_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    graphics_desc.DepthStencilState.BackFace = graphics_desc.DepthStencilState.FrontFace;
    D3D12_INPUT_ELEMENT_DESC input_element{
        "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    graphics_desc.InputLayout = {&input_element, 1};
    graphics_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics_desc.NumRenderTargets = 1;
    graphics_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    graphics_desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> stored_graphics{};
    const auto create_graphics_result = device->CreateGraphicsPipelineState(
        &graphics_desc, IID_PPV_ARGS(&stored_graphics));
    const auto store_graphics_result = SUCCEEDED(create_graphics_result)
        ? pipeline_library->StorePipeline(L"graphics", stored_graphics.Get())
        : E_FAIL;
    if (FAILED(create_graphics_result) || FAILED(store_graphics_result)) {
        std::cerr << "could not seed graphics pipeline library entry: create=0x"
                  << std::hex << static_cast<uint32_t>(create_graphics_result)
                  << " store=0x" << static_cast<uint32_t>(store_graphics_result) << '\n';
        return 14;
    }

    ComPtr<ID3D12PipelineState> loaded_graphics{};
    if (FAILED(pipeline_library->LoadGraphicsPipeline(
            L"graphics", &graphics_desc, IID_PPV_ARGS(&loaded_graphics)))) {
        std::cerr << "LoadGraphicsPipeline failed\n";
        return 15;
    }

    ComputePipelineStream stream{};
    stream.root_signature.payload = root_signature.Get();
    stream.compute_shader.payload = desc.CS;
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};

    ComPtr<ID3D12PipelineState> stored_stream{};
    if (FAILED(device2->CreatePipelineState(
            &stream_desc, IID_PPV_ARGS(&stored_stream))) ||
        FAILED(pipeline_library->StorePipeline(L"stream", stored_stream.Get()))) {
        std::cerr << "could not seed stream pipeline library entry\n";
        return 16;
    }

    ComPtr<ID3D12PipelineState> loaded_stream{};
    if (FAILED(pipeline_library->LoadPipeline(
            L"stream", &stream_desc, IID_PPV_ARGS(&loaded_stream)))) {
        std::cerr << "LoadPipeline failed\n";
        return 17;
    }

    GraphicsPipelineStream graphics_stream{};
    graphics_stream.root_signature.payload = graphics_desc.pRootSignature;
    graphics_stream.vertex_shader.payload = graphics_desc.VS;
    graphics_stream.pixel_shader.payload = graphics_desc.PS;
    graphics_stream.blend.payload = graphics_desc.BlendState;
    graphics_stream.sample_mask.payload = graphics_desc.SampleMask;
    graphics_stream.rasterizer.payload = graphics_desc.RasterizerState;
    graphics_stream.depth_stencil.payload = graphics_desc.DepthStencilState;
    graphics_stream.input_layout.payload = graphics_desc.InputLayout;
    graphics_stream.topology.payload = graphics_desc.PrimitiveTopologyType;
    graphics_stream.render_targets.payload.NumRenderTargets = graphics_desc.NumRenderTargets;
    std::memcpy(
        graphics_stream.render_targets.payload.RTFormats,
        graphics_desc.RTVFormats,
        sizeof(graphics_desc.RTVFormats));
    graphics_stream.sample_desc.payload = graphics_desc.SampleDesc;
    D3D12_PIPELINE_STATE_STREAM_DESC graphics_stream_desc{
        sizeof(graphics_stream), &graphics_stream};

    ComPtr<ID3D12PipelineState> stored_graphics_stream{};
    if (FAILED(device2->CreatePipelineState(
            &graphics_stream_desc, IID_PPV_ARGS(&stored_graphics_stream))) ||
        FAILED(pipeline_library->StorePipeline(
            L"graphics_stream", stored_graphics_stream.Get()))) {
        std::cerr << "could not seed graphics stream pipeline library entry\n";
        return 18;
    }

    ComPtr<ID3D12PipelineState> loaded_graphics_stream{};
    if (FAILED(pipeline_library->LoadPipeline(
            L"graphics_stream", &graphics_stream_desc, IID_PPV_ARGS(&loaded_graphics_stream)))) {
        std::cerr << "graphics LoadPipeline failed\n";
        return 19;
    }

    EnumeratedRecords enumerated{
        reinterpret_cast<uintptr_t>(loaded_graphics.Get()),
        reinterpret_cast<uintptr_t>(loaded_compute.Get()),
        reinterpret_cast<uintptr_t>(loaded_stream.Get()),
        reinterpret_cast<uintptr_t>(loaded_graphics_stream.Get())};
    api->enumerate_records(&collect_record, &enumerated);
    if (enumerated.total != 16 ||
        !enumerated.saw_library_graphics ||
        !enumerated.saw_library_compute ||
        !enumerated.saw_library_stream ||
        !enumerated.saw_library_graphics_stream) {
        std::cerr << "pipeline-library or stream enumeration mismatch: total=" << enumerated.total
                  << " graphics=" << enumerated.saw_library_graphics
                  << " compute=" << enumerated.saw_library_compute
                  << " stream=" << enumerated.saw_library_stream
                  << " graphics_stream=" << enumerated.saw_library_graphics_stream << '\n';
        return 25;
    }

    auto** compute_slot = &(*reinterpret_cast<void***>(device.Get()))[11];
    DWORD old_protection{};
    if (!VirtualProtect(compute_slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protection)) {
        std::cerr << "could not install chained-hook simulation\n";
        return 26;
    }
    g_chained_compute_original = reinterpret_cast<CreateComputePipelineStateFn>(
        InterlockedExchangePointer(
            compute_slot,
            reinterpret_cast<void*>(&chained_create_compute_pipeline_state)));
    DWORD ignored_protection{};
    VirtualProtect(compute_slot, sizeof(void*), old_protection, &ignored_protection);

    std::atomic<bool> start_handoff_stress{};
    std::atomic<uint32_t> handoff_failures{};
    workers.clear();
    for (size_t i = 0; i < 8; ++i) {
        workers.emplace_back([&]() {
            while (!start_handoff_stress.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (size_t j = 0; j < 64; ++j) {
                ComPtr<ID3D12PipelineState> pipeline{};
                if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
                    handoff_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    start_handoff_stress.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds{1});

    if (api->release_creation_hooks == nullptr || !api->release_creation_hooks()) {
        std::cerr << "hook handoff failed\n";
        return 20;
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (handoff_failures.load(std::memory_order_relaxed) != 0) {
        std::cerr << "pipeline creation failed during hook handoff\n";
        return 21;
    }

    api->get_status(&status);
    const auto captured_at_handoff = status.compute_pipelines;
    const auto records_at_handoff = status.records;

    ComPtr<ID3D12PipelineState> post_handoff_pipeline{};
    if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&post_handoff_pipeline)))) {
        std::cerr << "pipeline creation failed after hook handoff\n";
        return 22;
    }

    ComPtr<ID3D12PipelineState> post_handoff_library_pipeline{};
    if (FAILED(pipeline_library->LoadComputePipeline(
            L"compute", &desc, IID_PPV_ARGS(&post_handoff_library_pipeline)))) {
        std::cerr << "pipeline-library load failed after hook handoff\n";
        return 23;
    }

    api->get_status(&status);
    if (status.hooks_active != 0 || status.state != UEVRShaderRegistryState_Disabled ||
        status.compute_pipelines != captured_at_handoff ||
        status.records != records_at_handoff ||
        g_chained_compute_calls.load(std::memory_order_relaxed) == 0) {
        std::cerr << "helper remained active after hook handoff\n";
        return 24;
    }

    std::cout << "shader registry bootstrap test passed\n";
    return 0;
}
