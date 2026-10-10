#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "SteamFrameBindings.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <atomic>
#endif

namespace uevr::steam_frame {
inline constexpr size_t max_saved_binding_bytes = 256 * 1024;
enum class SavedDefaults { Preserve, Current, Legacy };

inline nlohmann::json parse_saved_defaults(std::string_view text) {
    if (text.size() > max_saved_binding_bytes || text.find('\0') != std::string_view::npos) {
        throw std::runtime_error{"Invalid binding size or embedded null"};
    }
    std::vector<std::set<std::string>> keys;
    return nlohmann::json::parse(text, [&](int depth, nlohmann::json::parse_event_t event, auto& value) {
        if (depth > 16) { throw std::runtime_error{"Binding nesting exceeds limit"}; }
        if (event == nlohmann::json::parse_event_t::object_start) { keys.emplace_back(); }
        if (event == nlohmann::json::parse_event_t::object_end) { keys.pop_back(); }
        if (event == nlohmann::json::parse_event_t::key &&
            !keys.back().insert(value.template get<std::string>()).second) {
            throw std::runtime_error{"Duplicate binding key"};
        }
        return true;
    });
}

inline SavedDefaults classify_saved_defaults(std::string_view saved, std::string_view current) noexcept {
    try {
        const auto installed = parse_saved_defaults(saved);
        const auto defaults = parse_saved_defaults(current);
        if (defaults != make_openvr_bindings()) { return SavedDefaults::Preserve; }
        if (installed == defaults) { return SavedDefaults::Current; }
        if (installed == make_openvr_bindings(false)) { return SavedDefaults::Legacy; }
    } catch (...) {}
    return SavedDefaults::Preserve;
}

inline std::optional<std::string> read_saved_defaults(const std::filesystem::path& path) noexcept {
    try {
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path))) { return std::nullopt; }
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return std::nullopt;
        }
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { return std::nullopt; }
        BY_HANDLE_FILE_INFORMATION info{};
        const bool regular_unlinked_file = GetFileInformationByHandle(handle, &info) && info.nNumberOfLinks == 1 &&
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0;
        CloseHandle(handle);
        if (!regular_unlinked_file) { return std::nullopt; }
#endif
        std::ifstream file{path, std::ios::binary};
        if (!file) { return std::nullopt; }
        std::string saved(max_saved_binding_bytes + 1, '\0');
        file.read(saved.data(), static_cast<std::streamsize>(saved.size()));
        if (file.bad() || file.gcount() > static_cast<std::streamsize>(max_saved_binding_bytes)) { return std::nullopt; }
        saved.resize(static_cast<size_t>(file.gcount()));
        return saved;
    } catch (...) { return std::nullopt; }
}

#ifdef _WIN32
// Publish a complete file by same-directory rename, never by truncating the
// user's file. Recheck the original snapshot immediately before replacement.
inline bool publish_stock_defaults(const std::filesystem::path& path, std::string_view current,
    std::optional<std::string_view> expected = std::nullopt) noexcept {
    if (current.empty() || current.size() > max_saved_binding_bytes || path.filename() != binding_file) { return false; }
    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path temporary;
    bool created = false;
    const auto cleanup = [&]() {
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
        if (created) { DeleteFileW(temporary.c_str()); }
    };
    try {
        static std::atomic<uint64_t> sequence{};
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            temporary = path;
            temporary += L".uevr-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { created = true; break; }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) { return false; }
        }
        if (!created) { return false; }
        DWORD written{};
        if (!WriteFile(file, current.data(), static_cast<DWORD>(current.size()), &written, nullptr) ||
            written != current.size() || !FlushFileBuffers(file)) {
            cleanup();
            return false;
        }
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (expected) {
            const auto latest = read_saved_defaults(path);
            if (!latest || *latest != *expected) { cleanup(); return false; }
        }
        const auto flags = MOVEFILE_WRITE_THROUGH | (expected ? MOVEFILE_REPLACE_EXISTING : 0);
        const bool published = MoveFileExW(temporary.c_str(), path.c_str(), flags) != FALSE;
        cleanup();
        return published;
    } catch (...) {
        cleanup();
        return false;
    }
}

enum class MigrationResult { Preserved, Current, Created, Upgraded, Failed };
inline MigrationResult migrate_stock_defaults(const std::filesystem::path& path, std::string_view current) noexcept {
    try {
        if (path.filename() != binding_file || parse_saved_defaults(current) != make_openvr_bindings()) {
            return MigrationResult::Failed;
        }
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path, ec);
        if (status.type() == std::filesystem::file_type::not_found) {
            return publish_stock_defaults(path, current) ? MigrationResult::Created : MigrationResult::Failed;
        }
        if (ec) { return MigrationResult::Preserved; }
        const auto saved = read_saved_defaults(path);
        if (!saved) { return MigrationResult::Preserved; }
        switch (classify_saved_defaults(*saved, current)) {
        case SavedDefaults::Current: return MigrationResult::Current;
        case SavedDefaults::Legacy:
            return publish_stock_defaults(path, current, *saved) ? MigrationResult::Upgraded : MigrationResult::Failed;
        default: return MigrationResult::Preserved;
        }
    } catch (...) { return MigrationResult::Failed; }
}
#endif
}
