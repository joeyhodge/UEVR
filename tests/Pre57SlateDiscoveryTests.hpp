#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include "mods/vr/Pre57SlateDiscovery.hpp"
#include "mods/vr/Pre57SlateCode.hpp"

namespace pre57_slate_tests {
namespace s = uevr::pre57_slate;
struct Fixture {
    static constexpr uintptr_t base = 0x180000000, begin = base + 0x1000;
    static constexpr uintptr_t name = base + 0x9000, target = base + 0x8000;
    static constexpr uintptr_t decoy = base + 0x7000;
    static constexpr size_t registration = 0x20;
    std::vector<uint8_t> code = std::vector<uint8_t>(256, 0x90);
    std::vector<uint8_t> original = std::vector<uint8_t>(16, 0x90);
    std::array<char16_t, 19> text{};
    std::vector<uint8_t> callee{s::glory_raw_registration.begin(),s::glory_raw_registration.end()};
    bool readable{true}, executable{true};
    size_t reads{};

    Fixture() {
        constexpr std::u16string_view label{u"SlateOutputTexture"};
        std::copy(label.begin(), label.end(), text.begin());
        put_registration(registration);
        code.back() = 0xc3;
    }
    void put_registration(size_t offset) {
        constexpr std::array<uint8_t, 31> bytes{
            0x4c,0x8d,0x05,0,0,0,0, 0x4c,0x89,0x7c,0x24,0x48,
            0x4c,0x89,0xf9, 0x48,0x89,0x94,0x24,0xb8,0,0,0,
            0x45,0x31,0xc9, 0xe8,0,0,0,0};
        std::copy(bytes.begin(), bytes.end(), code.begin() + offset);
        const auto name_delta = static_cast<int32_t>(name - (begin + offset + 7));
        const auto call_delta = static_cast<int32_t>(target - (begin + offset + bytes.size()));
        std::memcpy(code.data() + offset + 3, &name_delta, 4);
        std::memcpy(code.data() + offset + 27, &call_delta, 4);
    }
    void insert_before_call(std::initializer_list<uint8_t> bytes) {
        const auto call = registration + 26;
        code.insert(code.begin() + call,bytes.begin(),bytes.end());
        const auto delta = static_cast<int32_t>(target - (begin + call + bytes.size() + 5));
        std::memcpy(code.data() + call + bytes.size() + 1,&delta,4);
    }
    s::ScanResult scan(s::ImageRange image = {base, 0x10000}, uintptr_t address = begin) {
        return s::find_raw_registration(code, address, image, original,
            {s::glory_raw_registration,s::glory_raw_registration_mask},
            [&](uintptr_t p, void* out, size_t count) {
                ++reads;
                if (!readable) { return false; }
                if (p == name && count == sizeof(text)) { std::memcpy(out, text.data(), count); return true; }
                if (p == target && count == callee.size()) { std::memcpy(out, callee.data(), count); return true; }
                if (p == decoy && count == callee.size()) { std::memset(out,0x90,count); return true; }
                return false;
            }, [&](uintptr_t p, size_t count) {
                return executable && ((p == address && count == code.size()) || ((p == target || p == decoy) && count <= callee.size()));
            });
    }
};
}

void test_pre57_slate_discovery() {
    namespace s = uevr::pre57_slate;
    using F = pre57_slate_tests::Fixture;
    const s::Owner owner{F::begin,F::base};
    expect(s::legacy_owner_matches(owner,F::begin,F::base,owner), "matching unwind owners remain accepted");
    expect(!s::legacy_owner_matches(owner,F::begin,F::base,s::Owner{F::begin + 0x100,F::base}) &&
        !s::legacy_owner_matches(owner,F::begin,F::base,s::Owner{F::begin,F::base + 0x100000}) &&
        !s::legacy_owner_matches(owner,F::begin,F::base,{}), "a known owner rejects neighboring, foreign or missing candidate ownership");
    expect(s::legacy_owner_matches({},F::begin,F::base,{}), "existing custom adapters retain their no-unwind behavior");
    expect(s::legacy_scan_limit({F::base,0x10000},F::begin,19) == 0x3000,
        "legacy adapters retain their scan budget after skipping complete hook instructions");
    expect(s::legacy_scan_limit({F::base,0x1100},F::begin,19) == 256,
        "legacy scan cannot leave the loaded module");
    expect(!s::legacy_scan_limit({F::base,0x10000},F::begin,0) &&
        !s::legacy_scan_limit({F::base,0x10000},F::begin,65) &&
        !s::legacy_scan_limit({F::base,0x1013},F::begin,19) &&
        !s::legacy_scan_limit({UINTPTR_MAX,0x10000},F::begin,19), "invalid legacy owner/hook bounds fail closed");
    expect(s::checked_extent(0, 0, 1920, 1080) == s::Extent{1920,1080}, "stock Slate extent is unchanged");
    expect(s::checked_extent(-1920, -1080, 0, 0) == s::Extent{1920,1080}, "negative window origins remain supported");
    expect(s::checked_extent(-10, -20, 16374, 16364) == s::Extent{16384,16384}, "maximum valid Slate extent remains supported");
    using I = std::numeric_limits<int32_t>;
    for (const auto rect : {std::array<int32_t,4>{0,0,0,1}, {0,0,1,0}, {1,0,0,1}, {0,1,1,0},
             {0,0,16385,1}, {0,0,1,16385}, {(I::min)(),0,(I::max)(),1}, {0,(I::min)(),1,(I::max)()},
             {(I::max)(),0,(I::min)(),1}, {0,(I::max)(),1,(I::min)()}}) {
        expect(!s::checked_extent(rect[0],rect[1],rect[2],rect[3]), "invalid/overflowing Slate rectangles fail before unsigned conversion");
    }
    F f;
    const auto saved = f.code;
    const auto result = f.scan();
    expect(result.match == s::Registration{F::begin + F::registration, F::name,
        F::begin + F::registration + 26, F::target}, "unique raw Slate registration preserves argument transport");
    expect(f.code == saved, "discovery only reads its source code");

    f.code[0] = 0xff; f.code[1] = 0x25;
    for (size_t i = 2; i < 14; ++i) { f.code[i] = 0xcc; }
    expect(f.scan().match == result.match, "far hook jump/address literal is replaced by complete original entry bytes");
    f = F{}; f.original = {0x48};
    expect(f.scan().state == s::ScanState::InvalidEntry, "partial stolen instruction cannot authorize discovery");
    f = F{}; f.original.clear();
    expect(f.scan().state == s::ScanState::InvalidEntry, "missing original hook bytes fail closed");
    f = F{}; f.original.resize(65);
    expect(f.scan().state == s::ScanState::InvalidEntry, "hook entry work is bounded");

    f = F{}; f.text.back() = u'X';
    expect(!f.scan().match, "a SlateOutputTexture prefix without the exact terminator is not authority");
    f = F{}; f.text[0] = u'x';
    expect(!f.scan().match, "a decoy texture name cannot select a call");
    f = F{}; f.readable = false;
    expect(!f.scan().match, "unreadable names cannot select a call");
    f = F{}; f.executable = false;
    expect(!f.scan().match, "non-executable source bytes are rejected");
    for (size_t i = 0; i < s::glory_raw_registration.size(); ++i) {
        f = F{}; f.callee[i] ^= 1;
        expect(f.scan().match.has_value() == (s::glory_raw_registration_mask[i] == 0),
            "callee ABI requires every original instruction byte except external displacements");
    }
    f = F{}; f.callee.resize(f.callee.size() - 1);
    expect(!f.scan().match, "a truncated callee cannot authorize the callsite");
    f = F{}; f.insert_before_call({0x49,0x89,0xd8});
    expect(!f.scan().match, "R8 clobber is rejected even when RCX and zero R9 remain valid");
    f = F{}; f.insert_before_call({0x31,0xd2});
    expect(!f.scan().match, "unproven texture argument writes invalidate the pending name transaction");
    f = F{}; f.insert_before_call({0x41,0xb9,1,0,0,0});
    expect(!f.scan().match, "nonzero flags overwrite invalidates previously observed zero flags");
    f = F{}; f.insert_before_call({0xeb,0});
    expect(!f.scan().match, "argument transport is not inferred across an unproven branch");
    f = F{}; f.insert_before_call({0xcd,0x2e});
    expect(!f.scan().match, "system/interrupt transport is never treated as a preserved argument block");
    f = F{};
    const auto decoy_delta = static_cast<int32_t>(F::decoy - (F::begin + F::registration + 31));
    std::memcpy(f.code.data() + F::registration + 27,&decoy_delta,4);
    expect(!f.scan().match, "a neighboring executable callee with similar argument registers still needs the complete known ABI");

    f = F{};
    const std::array<uint8_t,3> hidden_return{0x48,0x89,0xe1};
    std::copy(hidden_return.begin(),hidden_return.end(),f.code.begin() + F::registration + 12);
    expect(!f.scan().match, "stack/hidden-return RCX is not a raw builder ABI");
    f = F{}; f.code[F::registration + 25] = 0xc0;
    expect(!f.scan().match, "overwriting R8 after the name invalidates it");
    f = F{}; f.code[F::registration + 24] = 0x85;
    expect(!f.scan().match, "testing R9 does not prove zero registration flags");
    f = F{}; f.code[F::registration + 26] = 0xe9;
    expect(!f.scan().match, "a jump is not a registration call");
    f = F{};
    const int32_t outside = 0x400000;
    std::memcpy(f.code.data() + F::registration + 27, &outside, 4);
    expect(!f.scan().match, "a callee outside the owning image is not accepted");
    f = F{}; f.code.resize(F::registration + 30);
    expect(f.scan().state == s::ScanState::InvalidCode, "a truncated direct call cannot be hooked");
    f = F{}; f.code.back() = 0x0f;
    expect(f.scan().state == s::ScanState::InvalidCode, "a partial owner window cannot establish unique registration");
    f = F{}; f.put_registration(0x80);
    expect(f.scan().state == s::ScanState::Ambiguous, "two matching name transactions are rejected, not first-match selected");

    expect(s::legacy_scan_limit({F::base,0x80000000},F::begin,19) == 0x3000,
        "large valid PE images do not lose existing legacy support");
    for (const auto image : {s::ImageRange{}, {F::base,0}, {UINTPTR_MAX,0x10000}, {F::base,0x100000000ull}}) {
        f = F{};
        expect(f.scan(image).state == s::ScanState::InvalidRange && f.reads == 0, "invalid image arithmetic cannot reach a data read");
    }
    f = F{}; f.code.resize(0x10001);
    expect(f.scan().state == s::ScanState::InvalidRange, "full-function discovery has a fixed work budget");
}

void test_pre57_slate_fixture(const char* path) {
    namespace s = uevr::pre57_slate;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || file.tellg() > 512 * 1024) { expect(false, "bounded external Slate fixture"); return; }
    file.seekg(0);
    const auto json = nlohmann::json::parse(file);
    const auto number = [&](const char* key) { return std::stoull(json.at(key).get<std::string>(), nullptr, 16); };
    const auto decode = [&](const char* key) {
        const auto value = json.at(key).get<std::string>();
        std::vector<uint8_t> bytes;
        if (value.size() % 2 || value.size() > 0x20000) { return bytes; }
        for (size_t i = 0; i < value.size(); i += 2) { bytes.push_back(static_cast<uint8_t>(std::stoul(value.substr(i,2),nullptr,16))); }
        return bytes;
    };
    auto code = decode("body_hex");
    const auto original = decode("entry_hex"), text = decode("name_bytes_hex");
    const auto callee_code = decode("target_code_hex");
    const s::ImageRange image{number("image_base"),number("image_size")};
    const auto begin = number("function_begin"), target = number("target_address"), target_size = number("target_size");
    const auto check = [&]() {
        const auto result = s::find_raw_registration(code, begin, image, original,
            {s::glory_raw_registration,s::glory_raw_registration_mask},
            [&](uintptr_t p, void* output, size_t count) {
                if (p == number("name_address") && count == text.size()) { std::memcpy(output,text.data(),count); return true; }
                if (p == target && count == callee_code.size()) { std::memcpy(output,callee_code.data(),count); return true; }
                return false;
            }, [&](uintptr_t p, size_t count) {
                return (p == begin && count == code.size()) || (p >= target && p - target <= target_size && count <= target_size - (p - target));
            });
        expect(result.match && result.match->callsite == number("expected_callsite") && result.match->target == target,
            "real shipping Slate code proves its unique original raw transaction");
    };
    check();
    if (code.size() >= 14 && original.size() >= 14) {
        std::fill_n(code.begin(),14,0xcc); code[0] = 0xff; code[1] = 0x25;
        check();
    }
}
