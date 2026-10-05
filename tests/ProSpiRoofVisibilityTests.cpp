#include "mods/vr/ProSpiRoofVisibilityPolicy.hpp"
#include "mods/vr/ProSpiRoofVisibilityCode.hpp"
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

using namespace uevr::prospi::roof;
namespace {
void require(bool result, const char* message) {
    if (!result) { throw std::runtime_error(message); }
}
std::vector<uint8_t> hex(std::string_view text) {
    std::vector<uint8_t> bytes;
    for (size_t i{}; i < text.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(std::string{text.substr(i, 2)}, nullptr, 16)));
    }
    return bytes;
}
void names() {
    const wchar_t* stadiums[]{L"Tokyo", L"Fukuoka", L"Osaka", L"Nagoya", L"Hokkaido", L"Seibu",
        L"Chiba", L"Hiroshima", L"Jingu", L"Kobe", L"Koshien", L"Kurashiki", L"Matsuyama", L"Miyagi", L"Yokohama"};
    for (const auto stadium : stadiums) {
        const auto package = std::wstring{L"/Game/Stadiums/"} + stadium + L"/Meshes/Roof/SM_ST_Roof";
        require(stadium_roof(L"p03_outfield__00_roof_c.fbx(dtokyo)", L"p03_outfield__00_roof_c", package),
            "must not lock roof selection to Tokyo or fixed hashes");
        require(stadium_roof(L"CEILING_Panel_2", L"SM_ST_Ceiling_Panel", package), "case-insensitive ceiling token");
    }
    require(stadium_roof(L"p07_alpha_outfield__00_roof_hole.fbx(dtokyo)", L"p07_alpha_outfield__00_roof_hole",
        L"/Game/Stadiums/Tokyo/Meshes/Roof/roof_hole"), "captured roof-hole geometry is a roof, not a mask");
    require(!stadium_roof(L"roof_1", L"roof_1", L"/Game/Characters/roof"), "non-stadium roof asset rejected");
    require(!stadium_roof(L"roof_1", L"roof_1", L"/Game/StadiumsOther/Tokyo/roof"), "exact stadium package boundary");
    require(!stadium_roof(L"roof_1", L"stand_1", L"/Game/Stadiums/Tokyo/stand"), "component name alone insufficient");
    for (const auto name : {L"SM_ST_SoloPlayMaskMesh_2", L"StadiumMaskPPV", L"PP_COM_SkyColor", L"M_ST_BlackMask",
            L"p03_roof_mask", L"Roof_Proxy", L"roof_collision", L"waterproof_wall", L"rooftop_sign", L"crowd_1", L"pitcher_1"}) {
        require(!roof_name(name), "masks, proxies, partial-word matches and unrelated stadium objects untouched");
    }
}
void eligibility() {
    Eligibility good{true, true, true, true, true, true, true, true, false, false};
    require(good.accepted(), "current live stadium roof accepted");
    for (auto flag : {&Eligibility::enabled, &Eligibility::game_thread, &Eligibility::current_world,
            &Eligibility::current_component, &Eligibility::current_owner, &Eligibility::current_mesh,
            &Eligibility::current_level, &Eligibility::level_in_world}) {
        auto bad = good; bad.*flag = false;
        require(!bad.accepted(), "disable, wrong thread, GC/reused references and travel fail closed");
    }
    auto hidden = good; hidden.owner_hidden = true;
    require(!hidden.accepted(), "never unhide actor-hidden geometry");
    hidden = good; hidden.component_hidden = true;
    require(!hidden.accepted(), "never clear component HiddenInGame");
}
void background_names() {
    for (const auto stadium : {L"Tokyo", L"Fukuoka", L"Osaka", L"Nagoya", L"Hokkaido", L"Shimin", L"Miyagi"}) {
        const auto package = std::wstring{L"/Game/Stadiums/"} + stadium + L"/Meshes/Day/SM_p03_outfield_wall_R4";
        require(stadium_geometry(L"p03_outfield_wall_R4.fbx(dstadium)", L"SM_p03_outfield_wall_R4", package),
            "structural outfield selection does not lock to Tokyo");
        require(stadium_geometry(L"p03_outfield_stairs", L"SM_p03_outfield_stairs", package), "unadorned names accepted");
        require(!stadium_geometry(L"p03_outfield_wall_L1", L"SM_p03_outfield_wall_R4", package),
            "mismatched component/mesh structural names rejected");
    }
    for (const auto name : {L"p03_outfield_wall_score", L"p03_outfield_stand_extra", L"p03_outfield_wall_wbc",
            L"p03_outfield_wall_displayTemplete", L"p03_outfield_wall_mask", L"p03_outfield_wall_proxy",
            L"p03_outfield_wall_collision", L"p03_outfield_wall_lod", L"p03_outfield_wall_variant",
            L"p03_outfield_wall_spectator", L"p03_outfield_adv_dorna4", L"p07_alpha_outfield__07_aurora",
            L"p02_infield_wall", L"p03_notoutfield_wall", L"p03_outfield_wallpaper"}) {
        require(!stadium_geometry(name, name, L"/Game/Stadiums/Tokyo/Meshes/Day/mesh"),
            "inactive variants, scoreboards, ads, masks and non-outfield assets excluded");
    }
    require(!stadium_geometry(L"p03_outfield_wall_R4", L"SM_p03_outfield_wall_R4", L"/Game/Characters/wall"),
        "structural name does not bypass package scope");

    for (const auto name : {L"p07_alpha_outfield__07_wallGrazed02", L"p03_outfield_of_C_rubberFence01",
            L"p03_outfield_std_1st_pillar1", L"p07_alpha_outfield__01_soratoshiba_handrail05",
            L"p07_alpha_outfield__09_std_C_window01", L"p07_alpha_outfield__07_of_home_fence01"}) {
        const auto mesh = std::wstring{L"SM_"} + name;
        const auto package = std::wstring{L"/Game/Stadiums/Hokkaido/Meshes/Night/"} + mesh;
        require(geometry(std::wstring{name} + L".fbx(nstadium)", mesh, package) == Geometry::structural,
            "captured numbered/compound structural roles recognized");
    }
    for (const auto name : {L"p00_soto__00_distant_view", L"p00_soto__00_field_out01",
            L"p03_outfield_std_R_1F_shop01", L"p07_alpha_outfield__01_alpha_outfield034",
            L"p07_alpha_outfield__14_2F_tower11logo", L"p02_infield_cheering"}) {
        const auto mesh = std::wstring{L"SM_"} + name;
        const auto package = std::wstring{L"/Game/Stadiums/AnotherStadium/Meshes/Day/"} + mesh;
        require(geometry(std::wstring{name} + L".fbx(dstadium)", mesh, package) == Geometry::observed_background,
            "background groups use game-visible proof, not a stadium/hash allowlist");
        require(geometry(name, mesh, L"/Game/StadiumsOther/Meshes/mesh") == Geometry::rejected,
            "background does not bypass exact package boundary");
        require(geometry(name, mesh, package + L"_another") == Geometry::rejected,
            "background package leaf must match the actual asset");
        require(geometry(std::wstring{name} + L"_another", mesh, package) == Geometry::rejected,
            "background component/mesh identities must agree");
    }
    for (const auto name : {L"p03_outfield_wallProxy01", L"p03_outfield_roofMask01", L"p03_outfield_wallLOD2",
            L"p03_outfield_wall_variant2", L"p03_outfield_wall_extra01", L"p03_outfield_wallSpectator01"}) {
        const auto mesh = std::wstring{L"SM_"} + name;
        const auto package = std::wstring{L"/Game/Stadiums/AnotherStadium/Meshes/Day/"} + mesh;
        require(geometry(name, mesh, package) == Geometry::rejected, "numbered/compound exclusions remain closed");
    }
}

void visibility_proof() {
    VisibilityProof pending{Geometry::observed_background};
    for (size_t i{}; i < 1000; ++i) {
        pending.sample_unforced(false);
        pending.game_request(false, false);
        require(!pending.allows_override(), "never-visible unknown variant cannot be forced by repeated discovery/hide calls");
    }
    pending.game_request(true, false);
    require(!pending.allows_override(), "unapplied game show request is not visibility proof");
    pending.game_request(false, true);
    require(!pending.allows_override(), "readback alone on a hide call is not a successful show request");
    pending.game_request(true, true);
    require(pending.allows_override(), "successful original game show call authorizes this background");
    for (const bool request : {false, true, false, true, false}) {
        pending.game_request(request, request);
        require(pending.allows_override(), "legitimate show proof survives camera cuts in the same identity/world");
    }
    VisibilityProof sampled{Geometry::observed_background};
    sampled.sample_unforced(true);
    require(sampled.allows_override(), "normally visible background can be learned at Draw");
    VisibilityProof rebound{Geometry::observed_background, false, true};
    for (size_t i{}; i < 1000; ++i) { rebound.sample_unforced(true); }
    require(!rebound.allows_override(), "a changed binding cannot learn from our previous forced visible readback");
    rebound.game_request(false, true);
    require(!rebound.allows_override(), "unsuccessful hide does not clear inherited-override evidence");
    rebound.game_request(true, false);
    require(!rebound.allows_override(), "unsuccessful show cannot authorize a new binding");
    rebound.game_request(true, true);
    require(rebound.allows_override(), "new binding requires its own successful game show");
    rebound = {Geometry::observed_background, false, true};
    rebound.sample_unforced(false); rebound.sample_unforced(true);
    require(rebound.allows_override(), "an observed real false/true transition supplies independent proof");
    pending = {Geometry::observed_background};
    require(!pending.allows_override(), "disable, travel or replaced identities never inherit another binding's proof");
    VisibilityProof structural{Geometry::structural};
    require(structural.allows_override(), "validated structural startup handling remains available");
    VisibilityProof excluded;
    excluded.sample_unforced(true); excluded.game_request(true, true);
    require(!excluded.allows_override(), "visibility never authorizes excluded masks/actors/assets");

    Lease lease;
    VisibilityProof unproven{Geometry::observed_background};
    require(disposable_pending(unproven, lease), "unproven discovery cannot crowd out structural targets at the cap");
    unproven.inherited_override = true;
    require(!disposable_pending(unproven, lease), "never forget a pending binding's inherited forced-value rejection");
    unproven.inherited_override = false; lease.forced = true;
    require(!disposable_pending(unproven, lease), "never evict restoration ownership");
    lease.forced = false; unproven.sample_unforced(true);
    require(!disposable_pending(unproven, lease), "never discard established visible proof for speculative discovery");
    require(!disposable_pending(structural, lease), "previous structural entries are never speculative evictions");
}
void maintenance() {
    size_t cursor = 999;
    require(maintenance_index(cursor, 0) == 0 && cursor == 0, "empty/travel cache resets cursor");
    for (size_t count : {size_t{1}, size_t{7}, size_t{36}, max_targets}) {
        cursor = 0;
        std::vector<bool> visited(count);
        for (size_t i{}; i < count; ++i) { visited[maintenance_index(cursor, count)] = true; }
        require(std::all_of(visited.begin(), visited.end(), [](bool v) { return v; }), "bounded sweep visits every target");
        require(cursor == 0, "bounded sweep wraps exactly");
    }
    cursor = max_targets;
    require(maintenance_index(cursor, 3) < 3 && cursor < 3, "cache shrink or erase never indexes out of bounds");
    require(cached_targets_per_draw < max_targets, "maintenance never validates the entire capped cache in one Draw");
}
void captured_stadium(const char* path, size_t components, size_t changes, size_t hidden_variants) {
    std::ifstream file{path};
    require(file.good(), "captured stadium fixture readable");
    const auto fixture = nlohmann::json::parse(file);
    size_t changed{}, direct{}, backgrounds{}, preserved_hidden{}, covered{}, learned_changes{};
    const auto widen = [](const std::string& value) { return std::wstring{value.begin(), value.end()}; };
    for (const auto& row : fixture.at("rows")) {
        const auto c = widen(row.at("component").get<std::string>());
        const auto m = widen(row.at("mesh").get<std::string>());
        const auto p = widen(row.at("package").get<std::string>());
        const auto before = row.at("behind_pitcher").get<bool>();
        const auto after = row.at("behind_plate").get<bool>();
        const auto kind = geometry(c, m, p);
        VisibilityProof proof{kind};
        proof.sample_unforced(before);
        if (row.contains("expected") && row.at("expected").get<bool>()) {
            require(kind == Geometry::structural, "previously accepted structural handling retained");
        }
        if (kind == Geometry::observed_background && !before) {
            require(!proof.allows_override(), "hidden background is not speculatively forced at startup");
        }
        changed += before != after;
        direct += kind == Geometry::structural;
        backgrounds += kind == Geometry::observed_background;
        proof.game_request(after, after);
        if (!after) {
            require(!proof.allows_override(), "no permanently hidden variant is forced visible");
            ++preserved_hidden;
        }
        if (before != after) {
            if (kind == Geometry::rejected || !proof.allows_override()) {
                throw std::runtime_error("captured camera-hidden background missed: " + row.at("component").get<std::string>());
            }
            require(!row.at("hidden_in_game").get<bool>(), "captured background must not be HiddenInGame");
            if (row.contains("actor_hidden")) { require(!row.at("actor_hidden").get<bool>(), "captured actor remains visible"); }
            proof.game_request(false, false);
            require(proof.allows_override(), "next behind-pitcher/fielding cut retains learned coverage");
            ++covered;
            learned_changes += kind == Geometry::observed_background;
        }
    }
    require(fixture.at("rows").size() == components && changed == changes, "complete controlled native A/B fixture retained");
    require(covered == changes && preserved_hidden == hidden_variants, "every captured camera-hidden component covered, all never-visible variants preserved");
    require(direct + backgrounds <= max_targets, "full captured stadium fits the unchanged bounded cache");
    std::cout << components << " components: " << covered << " camera changes covered (" << learned_changes
        << " require visible proof), " << preserved_hidden << " hidden variants preserved, "
        << direct << " direct / " << backgrounds << " background candidates\n";
}
void cuts_and_restore() {
    Lease lease;
    lease.game_request(true);
    require(!lease.should_restore(true), "naturally visible roof not owned by guard");
    // Behind pitcher, fielding, normal camera, celebration, replay: no camera
    // mode or coordinates participate in eligibility or visibility decisions.
    for (const bool requested : {false, false, true, false, true, false}) {
        lease.game_request(requested);
        lease.override_applied(true);
        require(lease.requested == requested, "remember exact latest game intent");
        require(lease.should_restore(true) == !requested, "only restore a forced value");
    }
    require(!lease.requested && lease.should_restore(true), "disable in hidden camera restores false");
    require(!lease.should_restore(false), "do not clobber an external change");
    lease.game_request(true);
    require(!lease.should_restore(true), "normal-camera request cancels stale hidden baseline");
    lease.game_request(false); lease.override_applied(false);
    require(!lease.should_restore(true), "failed readback never publishes an override lease");
    lease = {};
    require(!lease.should_restore(true), "new stadium never inherits previous-world restoration state");
}
void setter_validation() {
    // Exact bounded entry captured from this game's source-confirmed native
    // setter. RIP-relative cookie relocation is irrelevant; flag accesses are not.
    const auto captured = hex(
        "48895c2418488974242055574156488d6c24b94881ec00010000488b05e7500d0e4833c448894537"
        "4180f802488bf90fb6894c010000410fb6f00f94c30fb6c1c0e805440fb6f224013ad074258d46ff3c01"
        "0fb6c20f96c380e1dfc0e0050ac8488b07888f4c010000488bcfff9070040000");
    require(visibility_setter(captured, 0x14c, 0x20), "captured SetVisibility must be recognized");
    require(!visibility_setter(captured, 0x14d, 0x20), "wrong reflected field offset rejected");
    for (const auto mask : {uint8_t{0}, uint8_t{1}, uint8_t{0x10}, uint8_t{0x30}, uint8_t{0xff}}) {
        require(!visibility_setter(captured, 0x14c, mask), "wrong shift/clear mask rejected");
    }
    for (size_t size{}; size < captured.size(); ++size) {
        require(!visibility_setter(std::span{captured}.first(size), 0x14c, 0x20), "truncated setter rejected");
    }
    auto hooked = captured;
    hooked[0] = 0xe9;
    require(!visibility_setter(hooked, 0x14c, 0x20), "already-detoured function rejected");
    auto shifted = captured;
    const uint8_t store[]{0x88, 0x8f, 0x4c, 0x01, 0x00, 0x00};
    auto it = std::search(shifted.begin(), shifted.end(), std::begin(store), std::end(store));
    require(it != shifted.end(), "store fixture found");
    it[2] = 0x4d;
    require(!visibility_setter(shifted, 0x14c, 0x20), "mismatched store offset rejected");
    auto moved = captured;
    moved[29] ^= 0x7f;
    require(visibility_setter(moved, 0x14c, 0x20), "different cookie RVA remains valid");
}
void bool_metadata() {
    // Both live SetVisibility parameters are 01 00 01 ff at their metadata
    // tail, matching UE4.27 PropertyBool.cpp. ByteMask is not FieldMask.
    require(native_bool_storage(1, 0, 1, 0xff), "source/live native bool representation accepted");
    require(!native_bool_storage(1, 0, 0xff, 0xff), "incorrect ByteMask==FieldMask assumption rejected");
    require(!native_bool_storage(1, 0, 1, 1), "packed object bool is not a native parameter");
    require(!native_bool_storage(4, 0, 1, 0xff), "unexpected bool size rejected");
    require(!native_bool_storage(1, 1, 1, 0xff), "unexpected parameter byte offset rejected");
    require(!native_bool_storage(1, 0, 0, 0xff), "missing true-value bit rejected");
}
void viewport_dispatch() {
    // Captured actual UObject and its secondary FCommonViewportClient pointer.
    constexpr uintptr_t viewport = 0xec2ac100;
    require(viewport_dispatch_offset(viewport, viewport) == 0, "already-normalized callback remains unchanged");
    require(viewport_dispatch_offset(viewport + 0x28, viewport) == 0x28,
        "captured native Draw callback matches the active Engine.GameViewport");
    require(!viewport_dispatch_offset(viewport + 0x28, viewport + 0x1000), "another engine's viewport rejected");
    require(!viewport_dispatch_offset(viewport + 0x10, viewport), "unsupported interface offset rejected");
    require(!viewport_dispatch_offset(viewport - 0x28, viewport), "backwards guess rejected");
    require(!viewport_dispatch_offset(viewport + 1, viewport + 1), "unaligned pointers rejected");
    require(!viewport_dispatch_offset(0, viewport) && !viewport_dispatch_offset(viewport, 0), "null pointers rejected");
    require(!viewport_dispatch_offset(0x20, UINTPTR_MAX - 7), "secondary-offset overflow cannot alias a low address");
    InitializationState state;
    require(!state.attempted && !state.supported, "initial setup starts unattempted");
    state.attempted = true;
    state.on_disabled(false);
    require(!state.attempted, "explicit off/on retries failed discovery");
    state.attempted = true;
    state.on_disabled(true);
    require(state.attempted, "never recreate a retained installed hook");
    state.supported = true;
    state.on_disabled(false);
    require(state.attempted && state.supported, "successful setup persists through off/on");
}
}
int main(int argc, char** argv) {
    try {
        names(); background_names(); visibility_proof(); eligibility(); maintenance(); cuts_and_restore(); setter_validation(); bool_metadata(); viewport_dispatch();
        require(argc == 3, "both captured stadium fixture arguments required");
        captured_stadium(argv[1], 366, 98, 32);
        captured_stadium(argv[2], 503, 185, 10);
        std::cout << "ProSpi stadium visibility classification, viewport dispatch, bounded maintenance, scope, cut/restore and native setter tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
