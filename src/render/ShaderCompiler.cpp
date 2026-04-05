#include "render/ShaderCompiler.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>

#include <Windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;

std::wstring to_wstring(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return std::wstring{value.begin(), value.end()};
    }

    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

std::string hr_to_string(HRESULT hr) {
    std::ostringstream ss{};
    ss << "HRESULT 0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return ss.str();
}

int shader_model_major(std::string_view profile) {
    const auto underscore = profile.find('_');
    if (underscore == std::string_view::npos || underscore + 1 >= profile.size()) {
        return 0;
    }

    const auto major_char = profile[underscore + 1];
    if (major_char < '0' || major_char > '9') {
        return 0;
    }

    return major_char - '0';
}

using DxcCreateInstanceProc = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

struct DxcRuntime {
    std::once_flag init_once{};
    HMODULE dxil_module{};
    HMODULE dxcompiler_module{};
    ComPtr<IDxcUtils> utils{};
    ComPtr<IDxcCompiler3> compiler{};
    std::filesystem::path loaded_from{};
    std::string failure_reason{};

    ~DxcRuntime() {
        compiler.Reset();
        utils.Reset();

        if (dxcompiler_module != nullptr) {
            FreeLibrary(dxcompiler_module);
        }

        if (dxil_module != nullptr) {
            FreeLibrary(dxil_module);
        }
    }

    static DxcRuntime& instance() {
        static DxcRuntime runtime{};
        return runtime;
    }

    bool ensure_loaded() {
        std::call_once(init_once, [this] { load(); });
        return compiler != nullptr && utils != nullptr;
    }

    std::vector<std::filesystem::path> candidate_directories() {
        std::vector<std::filesystem::path> dirs{};

        wchar_t module_path[MAX_PATH]{};
        const auto module_len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        if (module_len > 0) {
            dirs.emplace_back(std::filesystem::path(std::wstring_view{module_path, module_len}).parent_path());
        }

        wchar_t env_buffer[32768]{};
        if (const auto env_len = GetEnvironmentVariableW(L"UEVR_DXC_PATH", env_buffer, std::size(env_buffer)); env_len > 0 && env_len < std::size(env_buffer)) {
            auto env_path = std::filesystem::path(std::wstring_view{env_buffer, env_len});
            dirs.emplace_back(std::filesystem::is_directory(env_path) ? env_path : env_path.parent_path());
        }

        const std::filesystem::path windows_kits_root{L"C:\\Program Files (x86)\\Windows Kits\\10"};
        const auto bin_root = windows_kits_root / "bin";
        if (std::filesystem::exists(bin_root)) {
            std::vector<std::filesystem::path> version_dirs{};
            for (const auto& entry : std::filesystem::directory_iterator(bin_root)) {
                if (!entry.is_directory()) {
                    continue;
                }

                version_dirs.emplace_back(entry.path());
            }

            std::sort(version_dirs.begin(), version_dirs.end(), std::greater<>{});
            for (const auto& version_dir : version_dirs) {
                dirs.emplace_back(version_dir / "x64");
            }
        }

        const auto redist_root = windows_kits_root / "Redist" / "D3D";
        if (std::filesystem::exists(redist_root)) {
            dirs.emplace_back(redist_root / "x64");
            std::vector<std::filesystem::path> redist_version_dirs{};
            for (const auto& entry : std::filesystem::directory_iterator(redist_root)) {
                if (!entry.is_directory()) {
                    continue;
                }
                redist_version_dirs.emplace_back(entry.path());
            }

            std::sort(redist_version_dirs.begin(), redist_version_dirs.end(), std::greater<>{});
            for (const auto& version_dir : redist_version_dirs) {
                dirs.emplace_back(version_dir / "x64");
            }
        }

        dirs.erase(std::remove_if(dirs.begin(), dirs.end(), [](const auto& dir) {
            return dir.empty();
        }), dirs.end());
        dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());

        return dirs;
    }

    void load() {
        for (const auto& dir : candidate_directories()) {
            const auto dxcompiler_path = dir / "dxcompiler.dll";
            if (!std::filesystem::exists(dxcompiler_path)) {
                continue;
            }

            const auto dxil_path = dir / "dxil.dll";
            if (std::filesystem::exists(dxil_path)) {
                dxil_module = LoadLibraryW(dxil_path.c_str());
            }

            dxcompiler_module = LoadLibraryW(dxcompiler_path.c_str());
            if (dxcompiler_module == nullptr) {
                if (dxil_module != nullptr) {
                    FreeLibrary(dxil_module);
                    dxil_module = nullptr;
                }
                continue;
            }

            const auto create_instance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxcompiler_module, "DxcCreateInstance"));
            if (create_instance == nullptr) {
                failure_reason = "dxcompiler.dll is missing DxcCreateInstance";
                FreeLibrary(dxcompiler_module);
                dxcompiler_module = nullptr;
                if (dxil_module != nullptr) {
                    FreeLibrary(dxil_module);
                    dxil_module = nullptr;
                }
                continue;
            }

            HRESULT hr = create_instance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
            if (FAILED(hr)) {
                failure_reason = "Failed to create IDxcUtils: " + hr_to_string(hr);
                FreeLibrary(dxcompiler_module);
                dxcompiler_module = nullptr;
                if (dxil_module != nullptr) {
                    FreeLibrary(dxil_module);
                    dxil_module = nullptr;
                }
                utils.Reset();
                continue;
            }

            hr = create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
            if (FAILED(hr)) {
                failure_reason = "Failed to create IDxcCompiler3: " + hr_to_string(hr);
                compiler.Reset();
                utils.Reset();
                FreeLibrary(dxcompiler_module);
                dxcompiler_module = nullptr;
                if (dxil_module != nullptr) {
                    FreeLibrary(dxil_module);
                    dxil_module = nullptr;
                }
                continue;
            }

            loaded_from = dxcompiler_path;
            failure_reason.clear();
            return;
        }

        failure_reason = failure_reason.empty() ? "DXC runtime not found" : failure_reason;
    }
};

render::ShaderCompileResult compile_with_dxc(const render::ShaderCompileRequest& request) {
    render::ShaderCompileResult result{};
    result.compiler = "dxc";

    auto& runtime = DxcRuntime::instance();
    if (!runtime.ensure_loaded()) {
        result.error = runtime.failure_reason;
        return result;
    }

    UINT32 code_page = DXC_CP_UTF8;
    ComPtr<IDxcBlobEncoding> source_blob{};
    auto hr = runtime.utils->LoadFile(request.source_path.c_str(), &code_page, &source_blob);
    if (FAILED(hr) || source_blob == nullptr) {
        result.error = "DXC failed to load " + request.source_path.string() + ": " + hr_to_string(hr);
        return result;
    }

    std::vector<std::wstring> arg_storage{};
    std::vector<LPCWSTR> args{};
    auto push_arg = [&](std::wstring value) {
        arg_storage.emplace_back(std::move(value));
        args.emplace_back(arg_storage.back().c_str());
    };

    push_arg(request.source_path.wstring());
    push_arg(L"-E");
    push_arg(to_wstring(request.entry_point));
    push_arg(L"-T");
    push_arg(to_wstring(request.profile));
    push_arg(L"-HV");
    push_arg(L"2021");

    if (request.warnings_as_errors) {
        push_arg(L"-WX");
    }

    if (request.strict_mode) {
        push_arg(L"-Ges");
        push_arg(L"-Zpc");
    }

    if (request.debug_info) {
        push_arg(L"-Zi");
    }

    if (request.strip_reflection) {
        push_arg(L"-Qstrip_reflect");
    }

    if (request.strip_debug) {
        push_arg(L"-Qstrip_debug");
    }

    ComPtr<IDxcIncludeHandler> include_handler{};
    hr = runtime.utils->CreateDefaultIncludeHandler(&include_handler);
    if (FAILED(hr) || include_handler == nullptr) {
        result.error = "DXC failed to create include handler: " + hr_to_string(hr);
        return result;
    }

    const DxcBuffer source_buffer{
        .Ptr = source_blob->GetBufferPointer(),
        .Size = source_blob->GetBufferSize(),
        .Encoding = code_page
    };

    ComPtr<IDxcResult> compile_result{};
    hr = runtime.compiler->Compile(&source_buffer, args.data(), static_cast<uint32_t>(args.size()), include_handler.Get(), IID_PPV_ARGS(&compile_result));
    if (FAILED(hr) || compile_result == nullptr) {
        result.error = "DXC compile call failed: " + hr_to_string(hr);
        return result;
    }

    HRESULT status = E_FAIL;
    compile_result->GetStatus(&status);

    ComPtr<IDxcBlobUtf8> errors{};
    if (SUCCEEDED(compile_result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors != nullptr && errors->GetStringLength() > 0) {
        result.notes.assign(errors->GetStringPointer(), errors->GetStringLength());
    }

    if (FAILED(status)) {
        result.error = !result.notes.empty() ? result.notes : ("DXC compile failed: " + hr_to_string(status));
        result.notes.clear();
        return result;
    }

    ComPtr<IDxcBlob> object{};
    hr = compile_result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    if (FAILED(hr) || object == nullptr) {
        result.error = "DXC returned no object output: " + hr_to_string(hr);
        return result;
    }

    result.bytecode.assign(
        static_cast<const uint8_t*>(object->GetBufferPointer()),
        static_cast<const uint8_t*>(object->GetBufferPointer()) + object->GetBufferSize()
    );
    result.succeeded = true;
    result.notes = "Loaded DXC from " + runtime.loaded_from.string();
    return result;
}

render::ShaderCompileResult compile_with_fxc(const render::ShaderCompileRequest& request) {
    render::ShaderCompileResult result{};
    result.compiler = "fxc";

    ComPtr<ID3DBlob> shader_blob{};
    ComPtr<ID3DBlob> error_blob{};

    UINT flags = 0;
    if (request.strict_mode) {
        flags |= D3DCOMPILE_ENABLE_STRICTNESS;
    }
    if (request.debug_info) {
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    } else {
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }
    if (request.warnings_as_errors) {
        flags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
    }

    const auto hr = D3DCompileFromFile(
        request.source_path.wstring().c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        request.entry_point.c_str(),
        request.profile.c_str(),
        flags,
        0,
        &shader_blob,
        &error_blob
    );

    if (FAILED(hr) || shader_blob == nullptr) {
        if (error_blob != nullptr && error_blob->GetBufferPointer() != nullptr) {
            result.error.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        } else {
            result.error = "FXC compile failed: " + hr_to_string(hr);
        }
        return result;
    }

    result.bytecode.assign(
        static_cast<const uint8_t*>(shader_blob->GetBufferPointer()),
        static_cast<const uint8_t*>(shader_blob->GetBufferPointer()) + shader_blob->GetBufferSize()
    );
    result.succeeded = true;
    return result;
}

} // namespace

namespace render {
ShaderCompileResult compile_shader_file(const ShaderCompileRequest& request) {
    ShaderCompileResult result{};

    if (request.source_path.empty()) {
        result.error = "Shader source path is empty";
        return result;
    }

    const auto major = shader_model_major(request.profile);

    auto try_dxc = [&]() {
        return compile_with_dxc(request);
    };

    auto try_fxc = [&]() {
        return compile_with_fxc(request);
    };

    switch (request.preferred_backend) {
    case ShaderCompilerBackend::Dxc:
        return try_dxc();
    case ShaderCompilerBackend::Fxc:
        return try_fxc();
    case ShaderCompilerBackend::Auto:
    default:
        break;
    }

    if (major >= 6) {
        return try_dxc();
    }

    return try_fxc();
}
} // namespace render
