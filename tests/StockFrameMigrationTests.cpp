#define NOMINMAX
#include "mods/vr/StockFrameBindingMigration.hpp"

#include <chrono>
#include <iostream>

namespace sf = uevr::steam_frame;
using nlohmann::json;
namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
uint64_t hash(std::string_view text) {
    uint64_t value = 14695981039346656037ull;
    for (const unsigned char byte : text) { value = (value ^ byte) * 1099511628211ull; }
    return value;
}

void classification() {
    const auto current = sf::make_openvr_bindings();
    const auto legacy = sf::make_openvr_bindings(false);
    const auto current_text = current.dump(4);
    const auto legacy_text = legacy.dump(4);
    // Independent canonical snapshot of the initial 75cefff7 stock generator.
    expect(hash(legacy.dump()) == 0x1458afc51ee4c4afull, "legacy recognition stays pinned to the shipped stock mapping");
    expect(sf::classify_saved_defaults(legacy_text, current_text) == sf::SavedDefaults::Legacy, "complete old stock upgrades");
    expect(sf::classify_saved_defaults(current_text, current_text) == sf::SavedDefaults::Current, "current stock is not rewritten");
    expect(sf::classify_saved_defaults(" \n" + legacy.dump() + "\r\n", current_text) == sf::SavedDefaults::Legacy,
        "stock whitespace and line endings do not prevent migration");

    for (const auto& base : {legacy, current}) {
        auto custom = base;
        custom["name"] = "My Frame bindings";
        expect(sf::classify_saved_defaults(custom.dump(), current_text) == sf::SavedDefaults::Preserve, "custom metadata is preserved");
        custom = base;
        custom["bindings"]["/actions/default"]["sources"][0]["inputs"]["click"]["output"] = "/actions/default/in/trigger";
        expect(sf::classify_saved_defaults(custom.dump(), current_text) == sf::SavedDefaults::Preserve, "a single remap is preserved");
        custom = base;
        custom["bindings"]["/actions/default"]["haptics"][0]["path"] = "/user/hand/right/output/haptic";
        expect(sf::classify_saved_defaults(custom.dump(), current_text) == sf::SavedDefaults::Preserve, "custom haptics are preserved");
        custom = base;
        custom["bindings"]["/actions/default"]["poses"][0]["path"] = "/user/hand/left/pose/raw";
        expect(sf::classify_saved_defaults(custom.dump(), current_text) == sf::SavedDefaults::Preserve, "custom poses are preserved");
        custom = base;
        custom["bindings"]["/actions/default"]["skeleton"] = json::array();
        expect(sf::classify_saved_defaults(custom.dump(), current_text) == sf::SavedDefaults::Preserve, "custom skeletons are preserved");
        custom = base;
        custom["extra"] = true;
        expect(sf::classify_saved_defaults(custom.dump(), current_text) == sf::SavedDefaults::Preserve, "unknown fields are preserved");
        custom = base;
        auto& sources = custom["bindings"]["/actions/default"]["sources"];
        std::reverse(sources.begin(), sources.end());
        expect(sf::classify_saved_defaults(custom.dump(), current_text) == sf::SavedDefaults::Preserve, "non-stock source ordering is preserved");
    }
    auto partial = legacy;
    partial["bindings"]["/actions/default"]["sources"][0]["inputs"]["touch"]["output"] = "/actions/default/in/triggertouch";
    expect(sf::classify_saved_defaults(partial.dump(), current_text) == sf::SavedDefaults::Preserve, "partial/manual touch edits are not stock");
    const auto duplicate = std::string{"{\"name\":\"Custom\","} + legacy_text.substr(1);
    expect(sf::classify_saved_defaults(duplicate, current_text) == sf::SavedDefaults::Preserve, "duplicate top-level keys fail closed");
    const auto repeated_nested_key = std::string{"{\"custom\":{\"x\":1,\"x\":2},"} + legacy_text.substr(1);
    expect(sf::classify_saved_defaults(repeated_nested_key, current_text) == sf::SavedDefaults::Preserve, "duplicate nested keys fail closed");
    for (const auto& malformed : {std::string{"{"}, std::string{"null"}, std::string{"[]"}, legacy_text + '\0'}) {
        expect(sf::classify_saved_defaults(malformed, current_text) == sf::SavedDefaults::Preserve, "malformed/non-stock input is preserved");
    }
    const std::string huge(sf::max_saved_binding_bytes + 1, ' ');
    expect(sf::classify_saved_defaults(huge, current_text) == sf::SavedDefaults::Preserve, "saved files have a byte limit");
    expect(sf::classify_saved_defaults(legacy_text, huge) == sf::SavedDefaults::Preserve, "replacement files have a byte limit");
    const auto deep = std::string(40, '[') + "0" + std::string(40, ']');
    expect(sf::classify_saved_defaults(deep, current_text) == sf::SavedDefaults::Preserve, "deep JSON is rejected before unbounded recursion");
    expect(sf::classify_saved_defaults(legacy_text, partial.dump()) == sf::SavedDefaults::Preserve, "an unexpected replacement cannot authorize migration");
}

struct TestDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("uevr-frame-migration-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() {
        if (!std::filesystem::create_directory(path)) { throw std::runtime_error{"Cannot create isolated test directory"}; }
    }
    ~TestDirectory() {
        std::error_code ec;
        // Only our direct test files are removed, never a recursive computed path.
        for (const auto& item : std::filesystem::directory_iterator(path, ec)) { std::filesystem::remove(item.path(), ec); }
        std::filesystem::remove(path, ec);
    }
};
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file) { throw std::runtime_error{"Cannot write test fixture"}; }
}

void files() {
    TestDirectory directory;
    const auto path = directory.path / sf::binding_file;
    const auto current = sf::make_openvr_bindings().dump(4);
    const auto legacy = sf::make_openvr_bindings(false).dump(4);
    expect(!sf::read_saved_defaults(path), "missing saved file is handled without throwing");
    const std::string huge(sf::max_saved_binding_bytes + 1, ' ');
    write(path, huge);
    expect(!sf::read_saved_defaults(path), "file reads are bounded to 256 KiB");
    write(path, legacy);
    expect(sf::read_saved_defaults(path) == legacy, "bounded reader preserves the exact original bytes");

#ifdef _WIN32
    using Result = sf::MigrationResult;
    expect(sf::migrate_stock_defaults(path, current) == Result::Upgraded, "real Windows publication upgrades the old stock file");
    expect(sf::read_saved_defaults(path) == current, "published defaults are complete and exact");
    const auto timestamp = std::filesystem::last_write_time(path);
    expect(sf::migrate_stock_defaults(path, current) == Result::Current && std::filesystem::last_write_time(path) == timestamp,
        "current stock is untouched, including its timestamp");
    std::filesystem::remove(path);
    expect(sf::migrate_stock_defaults(path, current) == Result::Created && sf::read_saved_defaults(path) == current,
        "new files are created with the current defaults");

    auto custom_json = sf::make_openvr_bindings(false);
    custom_json["name"] = "Customized";
    const auto custom = custom_json.dump(4);
    for (const auto& saved : {custom, std::string{"{"}, huge}) {
        write(path, saved);
        expect(sf::migrate_stock_defaults(path, current) == Result::Preserved, "custom, malformed and oversized files are not replaced");
        std::ifstream file{path, std::ios::binary};
        expect(std::string(std::istreambuf_iterator<char>{file}, {}) == saved, "preservation is byte-for-byte");
    }
    write(path, custom);
    expect(!sf::publish_stock_defaults(path, current, legacy) && sf::read_saved_defaults(path) == custom,
        "a changed original aborts publication instead of overwriting it");
    expect(!sf::publish_stock_defaults(path, current) && sf::read_saved_defaults(path) == custom,
        "new-file publication cannot overwrite a file created concurrently");
    expect(sf::migrate_stock_defaults(path, "{") == Result::Failed && sf::read_saved_defaults(path) == custom,
        "invalid replacement leaves the saved file untouched");
    expect(sf::migrate_stock_defaults(directory.path / "other.json", current) == Result::Failed,
        "migration is restricted to the Frame filename");
    expect(sf::migrate_stock_defaults(directory.path / "missing" / sf::binding_file, current) == Result::Failed,
        "unwritable destinations fail without partial files");

    write(path, legacy);
    expect(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "read-only fixture is established");
    expect(sf::migrate_stock_defaults(path, current) == Result::Failed && sf::read_saved_defaults(path) == legacy,
        "read-only saved files survive a failed publication");
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    const auto locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    expect(locked != INVALID_HANDLE_VALUE, "locked fixture is established");
    expect(sf::migrate_stock_defaults(path, current) == Result::Failed && sf::read_saved_defaults(path) == legacy,
        "sharing violations leave the original intact");
    if (locked != INVALID_HANDLE_VALUE) { CloseHandle(locked); }

    const auto hardlink = directory.path / "linked.json";
    expect(CreateHardLinkW(hardlink.c_str(), path.c_str(), nullptr) != FALSE, "hard-link fixture is established");
    expect(sf::migrate_stock_defaults(path, current) == Result::Preserved, "intentional hard-linked defaults are not replaced");
    std::filesystem::remove(hardlink);
    const auto link_target = directory.path / "target.json";
    std::filesystem::rename(path, link_target);
    if (CreateSymbolicLinkW(path.c_str(), link_target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        expect(sf::migrate_stock_defaults(path, current) == Result::Preserved, "symlinked defaults are not replaced");
        expect(sf::read_saved_defaults(link_target) == legacy, "symlink target stays unchanged");
        std::filesystem::remove(path);
        std::filesystem::remove(link_target);
    } else {
        std::cout << "Symlink test unavailable without Developer Mode/privilege\n";
        std::filesystem::remove(link_target);
    }
    expect(std::filesystem::is_empty(directory.path), "successes and ordinary failures leave no temporary files");
#endif
}
}
int main() {
    try { classification(); files(); }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    if (failures == 0) { std::cout << "Stock Frame migration tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
