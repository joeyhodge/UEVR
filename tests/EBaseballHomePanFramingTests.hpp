#pragma once

#include "EBaseballFramingRepairTests.hpp"

namespace ebaseball_home_pan_tests {
namespace rp = uevr::prospi::repair;
namespace fg = uevr::prospi::focus_guard;
namespace tr = uevr::prospi::trace;
namespace helpers = ebaseball_framing_tests;

inline rp::Request request() {
    auto r = helpers::request();
    r.home_pan = true;
    r.camera.camera = {{350,1200,220},{0,-95,0},20,0,0,true};
    r.camera.zone = 3;
    r.camera.dolly = 950; r.camera.lift = 400;
    r.camera.floor = r.legacy_floor = 1705;
    return r;
}

template<class Expect> void run(Expect&& expect) {
    for (unsigned mask=0;mask<32;++mask) {
        const rp::HomePanGates gates{bool(mask&1),bool(mask&2),bool(mask&4),bool(mask&8),bool(mask&16)};
        expect(rp::admit_home_pan(request().camera.camera,gates) ==
            (gates.ebaseball && (gates.presentation || gates.pitch_label) && !gates.protected_preset && !gates.celebration),
            "a soft pitch label admits observation, but fixed gameplay, celebrations and other game variants stay protected");
    }
    // Synthetic focus planes exercise the geometry, not a claim of recorded
    // player identities or a collision-free route through stadium geometry.
    for (bool mirrored : {false,true}) for (float pitch : {-5.0f,0.0f,5.0f})
        for (float roll : {-20.0f,0.0f,20.0f}) for (float fov : {8.0f,20.0f,35.0f}) {
        auto r = request();
        r.camera.camera.rotation[0] = pitch;
        r.camera.camera.rotation[2] = roll;
        r.camera.camera.fov = fov;
        if (mirrored) {
            r.camera.camera.location[0] *= -1;
            r.camera.camera.rotation[1] = -85;
        }
        const auto source = helpers::source(r,1000);
        const auto d = rp::evaluate(r,source,3.3f);
        expect(d.applied && d.reason == rp::Reason::home_pan_height && d.lift < r.camera.lift &&
            rp::window_contains(r,source,d.dolly,d.lift,3.3f),
            "only a proved clipped home-pan focus sheds an excess stand lift, with the whole stereo window retained");
        helpers::verify_projection(r,source,d,3.3f,expect);
    }

    auto r = request(); r.camera.dolly = 300; r.camera.lift = 20;
    auto source = helpers::source(r,1000);
    auto d = rp::evaluate(r,source,3.3f);
    expect(!d.applied && d.dolly == 300 && d.lift == 20,
        "a well-framed home pan is bit-identical even when its stand-floor label is suspicious");
    r = request(); r.camera.zone = 0; r.camera.floor = r.legacy_floor = 0;
    r.camera.dolly = 1200; r.camera.lift = 0;
    source = helpers::source(r,1000); d = rp::evaluate(r,source,3.3f);
    expect(d.applied && d.reason == rp::Reason::home_pan_focus && d.dolly < 1000 &&
        rp::window_contains(r,source,d.dolly,d.lift,3.3f),
        "a home-pan soft preset without a floor label may repair a proved crossing without bypassing the real field floor");

    for (int bad = 0; bad < 26; ++bad) {
        r = request(); source = helpers::source(r,1000);
        switch (bad) {
        case 0:r.home_pan=false;break;
        case 1:r.ebaseball=false;break;
        case 2:r.manual_controls=true;break;
        case 3:r.protected_camera=true;break;
        case 4:r.camera.mode=2;break;
        case 5:r.camera.enabled=false;break;
        case 6:r.camera.safety_enabled=false;break;
        case 7:r.camera.sequencer_enabled=false;break;
        case 8:r.camera.automatic_source=false;break;
        case 9:r.camera.zone=4;break;
        case 10:r.camera.camera.location[0]=3500;break;
        case 11:r.camera.camera.location[1]=3501;break;
        case 12:r.camera.camera.location[2]=1500;break;
        case 13:r.camera.camera.rotation[1]=0;break;
        case 14:r.camera.camera.rotation[0]=-26;break;
        case 15:r.camera.camera.rotation[2]=26;break;
        case 16:r.camera.camera.fov=61;break;
        case 17:source.time_ns-=50'000'001;break;
        case 18:source.pose.location[0]+=2;break;
        case 19:source.camera_manager++;break;
        case 20:source.object=0;break;
        case 21:source.pose.aspect_valid=false;break;
        case 22:r.camera.forward_offset=1;break;
        case 23:r.camera.up_offset=1;break;
        case 24:r.field_floor=400;break;
        case 25:r.camera.protected_camera=true;break;
        }
        d = rp::evaluate(r,source,3.3f);
        expect(!d.applied && d.dolly == r.camera.dolly && d.lift == r.camera.lift,
            "manual/gameplay/foreign/stale or out-of-envelope home-pan requests keep the original endpoint");
    }

    // An aligned target outside the plate area must not authorize a field-floor
    // repair: it may be a real stand, distant subject, or gameplay view.
    r = request(); r.camera.camera = {{0,1800,500},{-4,-90,0},20,0,0,true};
    source = helpers::source(r,4200); d = rp::evaluate(r,source,3.3f);
    expect(!d.applied && d.reason == rp::Reason::source_mismatch,
        "an exact current source aimed beyond the plate-area target bounds cannot authorize a home-pan repair");
    r = request(); r.camera.camera.location[2]=500;
    source = helpers::source(r,1000); d = rp::evaluate(r,source,3.3f);
    expect(!d.applied && d.reason == rp::Reason::source_mismatch,
        "an aligned high target is not assumed to be a player at home plate");
    r = request(); r.camera.camera.location[0]=850; r.camera.camera.rotation[1]=-50;
    source = helpers::source(r,1500); d = rp::evaluate(r,source,3.3f);
    expect(!d.applied && d.reason == rp::Reason::source_mismatch,
        "an aligned target outside the near-plate lateral bounds keeps stadium protection");

    for (const tr::Pose pose : {
        tr::Pose{{3500,3000,1500},{-10,-125,0},8.35f,0,0,true},
        tr::Pose{{0,3000,1800},{-20,-90,0},20,0,0,true},
        tr::Pose{{0,3000,500},{-10,-100,0},15,0,0,true},
        tr::Pose{{0,-10000,500},{0,90,0},8,0,0,true},
        tr::Pose{{3200,-1400,220},{0,140,0},8,0,0,true}}) {
        r = request(); r.camera.camera=pose;
        source=helpers::source(r,1000); d=rp::evaluate(r,source,3.3f);
        expect(!d.applied && d.dolly==r.camera.dolly && d.lift==r.camera.lift,
            "a retained home-pan request cannot fall into another repair scope after a cut");
    }

    auto id = helpers::identity(); r=request(); source=helpers::source(r,1000);
    rp::Continuity continuity;
    const auto first=continuity.update(r,source,id,3.3f);
    expect(first.applied,"a current owned home-pan endpoint can acquire continuity");
    r.camera.now_ns+=16'666'667;
    r.camera.dolly=300; r.camera.lift=20; source=helpers::source(r,1000);
    const auto good=continuity.update(r,source,id,3.3f);
    expect(!good.applied && good.dolly==300 && good.lift==20,
        "home-pan continuity never holds an old correction over a currently good endpoint");
    r=request(); source=helpers::source(r,1000); id.manager.serial++;
    uevr::prospi::cut_guard::Snapshot snapshot{};
    snapshot.camera=r.camera.camera; snapshot.time_ns=r.camera.now_ns;
    snapshot.identity=helpers::identity(); snapshot.framing_repair=r;
    const auto original=uevr::prospi::cut_guard::Decision{
        uevr::prospi::cut_guard::Status::unchanged,r.camera.dolly,r.camera.lift};
    auto input=r.camera.camera; input.fov=0;
    const auto declined=uevr::prospi::cut_guard::repair_current(snapshot,input,r.camera.now_ns,
        id,source,3.3f,original,continuity);
    expect(declined.dolly==original.dolly && declined.lift==original.lift,
        "a retired camera-manager owner cannot repair a new home-pan view");
}
}
