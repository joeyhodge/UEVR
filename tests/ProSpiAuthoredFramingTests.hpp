#pragma once

#include "mods/vr/ProSpiAuthoredTargetGuard.hpp"
#include "mods/vr/ProSpiCameraCutGuard.hpp"
#include <limits>
#include <random>

namespace prospi_authored_framing_tests {
namespace fg = uevr::prospi::focus_guard;
namespace tr = uevr::prospi::trace;
namespace cg = uevr::prospi::cut_guard;
namespace shot = uevr::prospi::shot;

inline fg::Request request() {
    return {.camera = {{3200,-1400,220}, {0,140,0}, 8, 0, 0, true},
        .camera_manager = 0x100000, .now_ns = 1'000'000'000, .mode = 3, .zone = 1,
        .enabled = true, .prospi = true, .dolly_enabled = true, .sequencer_enabled = true,
        .safety_enabled = true, .automatic_source = true, .dolly = 1500, .lift = 0,
        .predicted_z = 220, .floor = 170, .max_lift = 400, .base_fov = 90};
}
inline tr::NativeSource source(const fg::Request& r, float focus = 900) {
    tr::NativeSource s{};
    s.pose = r.camera; s.pose.aspect = 16.0f/9; s.pose.aspect_valid = true;
    s.object = 0x200000; s.camera_manager = r.camera_manager; s.camera_actor = 0x300000;
    s.time_ns = r.now_ns; s.frame = 40; s.focus_distance = focus; s.status = tr::SourceStatus::accepted;
    const auto f = tr::forward(r.camera);
    for (size_t i = 0; i < 3; ++i) { s.look_at[i] = r.camera.location[i] + f[i] * focus; }
    return s;
}
inline tr::Pose endpoint(const fg::Request& r, const fg::Decision& d, const tr::NativeSource& s) {
    auto pose = r.camera; pose.fov = r.base_fov; pose.aspect = s.pose.aspect; pose.aspect_valid = true;
    const auto f = tr::forward(pose);
    constexpr double rad = 0.017453292519943295;
    const double pitch = pose.rotation[0]*rad, yaw = pose.rotation[1]*rad;
    const tr::Vector up{float(-std::sin(pitch)*std::cos(yaw)), float(-std::sin(pitch)*std::sin(yaw)), float(std::cos(pitch))};
    for (size_t i = 0; i < 3; ++i) { pose.location[i] += f[i]*d.dolly + up[i]*d.lift; }
    return pose;
}
template<class Expect>
void verify_window(const fg::Request& r, const tr::NativeSource& s, const fg::Decision& d, Expect&& expect) {
    const auto view = endpoint(r,d,s);
    const auto after = tr::target_framing(view,s.look_at);
    const auto before = tr::target_framing(endpoint(r,{.dolly=r.dolly,.lift=r.lift},s),s.look_at);
    expect(before.behind || before.offscreen_estimate, "authored-window repair requires a proven pre-existing framing failure");
    expect(d.dolly >= 0 && d.dolly <= r.dolly && d.lift >= 0 && d.lift <= r.max_lift &&
        view.location[2] + 0.003f >= r.floor && after.valid && !after.behind && !after.offscreen_estimate,
        "authored-window endpoint respects dolly, height, floor and target constraints");
    constexpr double rad = 0.017453292519943295;
    const auto fov_half = std::tan(s.pose.fov*rad*.5)*s.focus_distance;
    const auto yaw = s.pose.rotation[1]*rad, pitch = s.pose.rotation[0]*rad;
    const tr::Vector right{float(-std::sin(yaw)),float(std::cos(yaw)),0};
    const tr::Vector up{float(-std::sin(pitch)*std::cos(yaw)),float(-std::sin(pitch)*std::sin(yaw)),float(std::cos(pitch))};
    for (int x : {-1,1}) for (int y : {-1,1}) {
        auto corner = s.look_at;
        for (size_t i=0;i<3;++i) { corner[i] += float(right[i]*x*fov_half + up[i]*y*fov_half/s.pose.aspect); }
        const auto framed = tr::target_framing(view,corner);
        expect(framed.valid && !framed.behind && framed.clip_estimate_valid &&
            std::abs(framed.ndc_x) <= .952f && std::abs(framed.ndc_y) <= .952f,
            "all four corners of the original authored focus-plane window remain framed");
    }
}
template<class Expect> void run(Expect&& expect) {
    auto r = request(); auto s = source(r);
    auto d = fg::bound_authored_window(r,s,true);
    expect(d.applied(), "a presentation dolly past the authored subject is recovered");
    if (d.applied()) { verify_window(r,s,d,expect); }
    const auto stereo=fg::bound_authored_window(r,s,true,3.3f);
    expect(stereo.applied() && stereo.dolly<d.dolly, "a repair reserves room for the runtime's per-eye displacement");
    if (stereo.applied()) {
        const auto neutral=endpoint(r,stereo,s);
        constexpr double rad=0.017453292519943295;
        const tr::Vector right{float(-std::sin(r.camera.rotation[1]*rad)),float(std::cos(r.camera.rotation[1]*rad)),0};
        for (float side : {-1.0f,1.0f}) {
            auto eye=neutral;
            for(size_t i=0;i<3;++i){eye.location[i]+=right[i]*side*3.3f;}
            const auto target=tr::target_framing(eye,s.look_at);
            expect(target.valid && !target.behind && !target.offscreen_estimate, "both translated eye origins retain the authored focus");
        }
    }
    const auto invalid_eye=fg::bound_authored_window(r,s,true,std::numeric_limits<float>::quiet_NaN());
    expect(!invalid_eye.applied() && invalid_eye.dolly==r.dolly, "invalid runtime eye geometry cannot authorize a correction");
    r.dolly = 500; r.lift = 400;
    s=source(r); d=fg::bound_authored_window(r,s,true);
    expect(d.applied() && d.lift < r.lift, "an actually clipped interview focus can shed excess lift without changing good low shots");
    if (d.applied()) { verify_window(r,s,d,expect); }
    for (const auto offset : {tr::Vector{0,0,0}, tr::Vector{25000,-14000,0}}) for (float side : {-1.0f,1.0f}) {
        r=request(); r.camera.location[0] *= side; r.camera.rotation[1] = side > 0 ? 140.0f : 40.0f;
        for (size_t i=0;i<3;++i) {r.camera.location[i]+=offset[i];}
        s=source(r);d=fg::bound_authored_window(r,s,true);
        expect(d.applied(), "both baseline sides and translated stadium coordinates use authored geometry rather than guessed podium coordinates");
        if (d.applied()) { verify_window(r,s,d,expect); }
    }
    r=request(); r.dolly=850; s=source(r);
    d=fg::bound_authored_window(r,s,true);
    expect(!d.applied() && d.dolly==r.dolly && d.lift==r.lift,
        "an intentionally tight, still-in-front action zoom is preserved exactly");
    r.dolly=200;r.lift=50;s=source(r);d=fg::bound_authored_window(r,s,true);
    expect(!d.applied() && d.dolly==r.dolly && d.lift==r.lift, "a correctly framed shot is an exact no-op");
    for (int bad=0;bad<13;++bad) {
        r=request();s=source(r);
        switch(bad) {
        case 0:r.enabled=false;break;case 1:r.mode=2;break;case 2:r.protected_camera=true;break;
        case 3:r.automatic_source=false;break;case 4:r.zone=3;break;case 5:r.forward_offset=1;break;
        case 6:r.right_offset=1;break;case 7:r.up_offset=1;break;case 8:s.camera_manager++;break;
        case 9:s.time_ns-=50'000'001;break;case 10:s.pose.location[0]+=2;break;
        case 11:s.object=0;break;case 12:s.pose.aspect_valid=false;break;
        }
        d=fg::bound_authored_window(r,s,true);
        expect(!d.applied() && d.dolly==r.dolly && d.lift==r.lift, "off/manual/foreign/stale or unsupported framing data leaves the baseline unchanged");
    }
    r=request();s=source(r);d=fg::bound_authored_window(r,s,false);
    expect(!d.applied() && d.dolly==r.dolly && d.lift==r.lift, "protected gameplay/ball-follow/replay/celebration admission cannot move a camera");
    r.camera.location[2]=0;r.lift=0;r.max_lift=0;s=source(r);d=fg::bound_authored_window(r,s,true);
    expect(!d.applied() && d.dolly==r.dolly, "a framing correction never lowers the floor to manufacture a valid endpoint");
    std::mt19937 random(0x20261101);
    const auto value=[&](float lo,float hi){return std::uniform_real_distribution<float>(lo,hi)(random);};
    unsigned repaired{},unchanged{};
    for (unsigned i=0;i<5000;++i) {
        r=request();r.camera.location={value(-6000,6000),value(-5000,5000),value(200,1200)};
        r.camera.rotation={value(-10,10),value(-180,180),0};r.camera.fov=value(2,30);r.base_fov=value(60,110);
        const auto focus=value(100,7000);r.dolly=(std::min)(8000.0f,focus*value(.1f,1.4f));r.lift=value(0,300);
        s=source(r,focus);d=fg::bound_authored_window(r,s,true);
        if (d.applied()) {++repaired;verify_window(r,s,d,expect);}
        else {++unchanged;expect(d.dolly==r.dolly && d.lift==r.lift, "a declined randomized correction never partially changes the shot");}
    }
    expect(repaired>1000 && unchanged>1000, "randomized tests exercise both repairs and preserved good shots");
    r=request(); r.camera={{1300,-420,162},{4,-150,0},31.5f,0,0,true}; r.dolly=746; r.lift=0;
    s=source(r,267);
    const auto scope=fg::AuthoredScope::near_home_crossing;
    d=fg::bound_authored_window(r,s,true,4,scope);
    expect(d.applied(), "a recorded near-home interview envelope can recover a proved crossing despite its soft preset classification");
    if(d.applied()){verify_window(r,s,d,expect);}
    r.camera.fov=47.8f; s=source(r,267);
    d=fg::bound_authored_window(r,s,true,4,scope);
    expect(d.applied() && !fg::bound_authored_window(r,s,true).applied(),
        "the wider recorded interview is admitted only by the bounded crossing-only scope, not a global FOV relaxation");
    r.dolly=100; r.lift=200; s=source(r,267);
    d=fg::bound_authored_window(r,s,true,4,scope);
    expect(!d.applied() && d.dolly==r.dolly && d.lift==r.lift,
        "the new near-home scope cannot retune an in-front shot, even if a vertical clipping estimate exists");
    r.dolly=746; r.lift=0;
    for(int bad=0;bad<12;++bad){
        auto rejected=r; auto src=source(r,267); bool admitted=true;
        switch(bad){
        case 0:rejected.camera.location[0]=150;break;
        case 1:rejected.camera.fov=24;break;
        case 2:rejected.camera.location[2]=800;break;
        case 3:rejected.protected_camera=true;break;
        case 4:rejected.forward_offset=1;break;
        case 5:rejected.safety_enabled=false;break;
        case 6:rejected.automatic_source=false;break;
        case 7:src=source(r,1200);break;
        case 8:src.time_ns-=50'000'001;break;
        case 9:src.camera_manager++;break;
        case 10:src.pose.location[0]+=2;break;
        case 11:admitted=false;break;
        }
        const auto result=fg::bound_authored_window(rejected,src,admitted,4,scope);
        expect(!result.applied() && result.dolly==rejected.dolly && result.lift==rejected.lift,
            "central gameplay, celebration rigs, protected settings, long focus and uncertain sources cannot authorize an interview recovery");
    }
    r=request();s=source(r);d=fg::bound_authored_window(r,s,true);
    tr::Camera camera{};camera.input=r.camera;camera.safety_active=true;camera.safety_predicted_z=-1000;camera.safety_up=400;
    tr::View view{};view.input=r.camera;view.current_source=s;view.source_matches_input=true;
    view.neutral=endpoint(r,d,s);view.neutral_valid=true;
    for(size_t i=0;i<3;++i){view.forward_offset[i]=view.neutral.location[i]-view.input.location[i];}
    expect(tr::view_suspects(camera,view)==0, "current-view framing diagnostics do not reuse the preceding camera's target or height estimate");

    shot::Identity a{{0x10000,0x100,1,7},{0x20000,0x200,2,8},{0x30000,0x300,3,9},0x40000,0x50000,true,true,false};
    cg::Snapshot snapshot{.camera=request().camera,.dolly=1500,.lift=20,.time_ns=1'000'000'000,.automatic=true,.identity=a};
    const auto same=cg::evaluate(snapshot,snapshot.camera,snapshot.time_ns,&a);
    expect(same.dolly==snapshot.dolly && same.lift==snapshot.lift, "matching current shot identity preserves the existing correction");
    for (int field=0;field<7;++field) {
        auto b=a;
        switch(field){case 0:b.world.serial++;break;case 1:b.manager.serial++;break;case 2:b.view_target.serial++;break;
            case 3:b.view_target.address+=8;break;case 4:b.native_camera+=8;break;case 5:b.camera_actor+=8;break;case 6:b.cut=true;break;}
        const auto stale=cg::evaluate(snapshot,snapshot.camera,snapshot.time_ns,&b);
        expect(stale.status==cg::Status::identity_changed && stale.dolly==0 && stale.lift==0,
            "same-pose camera cuts and recycled world/manager/target identities cannot reuse the preceding shot's dolly");
    }
    auto unknown=a;unknown.valid=false;
    expect(cg::evaluate(snapshot,snapshot.camera,snapshot.time_ns,&unknown).dolly==snapshot.dolly,
        "unavailable identity evidence does not invent a cut or change an existing good shot");
    snapshot.presentation_request=request();
    snapshot.presentation_request.camera_manager=a.manager.address;
    auto fresh=source(snapshot.presentation_request);
    fresh.camera_manager=a.manager.address;
    snapshot.presentation=true;
    auto new_shot=a;new_shot.native_camera+=8;
    fresh.object=new_shot.native_camera;fresh.camera_actor=new_shot.camera_actor;
    auto in_front=snapshot;
    r=request();
    r.camera={{1300,-420,162},{4,-150,0},47.8f,0,0,true};
    r.camera_manager=a.manager.address; r.dolly=100; r.lift=200;
    in_front.camera=r.camera; in_front.presentation_request=r;
    in_front.dolly=r.dolly; in_front.lift=r.lift; in_front.presentation=true;
    in_front.scope=fg::AuthoredScope::near_home_crossing;
    auto in_front_source=source(r,267);
    in_front_source.object=new_shot.native_camera; in_front_source.camera_actor=new_shot.camera_actor;
    const auto retained=cg::evaluate_current(in_front,in_front.camera,in_front.time_ns,new_shot,in_front_source,4);
    expect(retained.dolly==in_front.dolly && retained.lift==in_front.lift && retained.status==cg::Status::unchanged,
        "crossing-only interview identity changes cannot reset in-front framing, including an offscreen estimate");
    const auto recovered=cg::evaluate_current(snapshot,snapshot.camera,snapshot.time_ns,new_shot,fresh);
    expect(recovered.status==cg::Status::authored_window && recovered.dolly<snapshot.dolly,
        "a same-pose new native camera with a crossed target receives a coherent current-source repair");
    auto good_snapshot=snapshot;good_snapshot.dolly=200;good_snapshot.lift=20;
    const auto revalidated=cg::evaluate_current(good_snapshot,snapshot.camera,snapshot.time_ns,new_shot,fresh);
    expect(revalidated.status==cg::Status::revalidated && revalidated.dolly==good_snapshot.dolly &&
        revalidated.lift==good_snapshot.lift, "a same-pose cut with already-good framing does not acquire a reset jump");
    for (int gate=0;gate<5;++gate) {
        auto protected_snapshot=good_snapshot;
        switch(gate) {
        case 0:protected_snapshot.presentation_request.enabled=false;break;
        case 1:protected_snapshot.presentation_request.protected_camera=true;break;
        case 2:protected_snapshot.presentation_request.forward_offset=1;break;
        case 3:protected_snapshot.presentation_request.safety_enabled=false;break;
        case 4:protected_snapshot.presentation_request.zone=4;break;
        }
        const auto unchanged=cg::evaluate_current(protected_snapshot,snapshot.camera,snapshot.time_ns,new_shot,fresh);
        expect(unchanged.dolly==good_snapshot.dolly && unchanged.lift==good_snapshot.lift &&
            unchanged.status==cg::Status::unchanged,
            "a camera identity hint cannot reset good framing outside the admitted automatic presentation guard");
    }
    auto wide_source=fresh;
    wide_source.pose.fov=45;
    const auto wide=cg::evaluate_current(good_snapshot,snapshot.camera,snapshot.time_ns,new_shot,wide_source);
    expect(wide.dolly==good_snapshot.dolly && wide.lift==good_snapshot.lift,
        "a wide authored shot outside the guard retains its original offsets even when native camera identity changes");
    fresh.status=tr::SourceStatus::unavailable;
    expect(cg::evaluate_current(good_snapshot,snapshot.camera,snapshot.time_ns,new_shot,fresh).dolly==good_snapshot.dolly,
        "unsupported native data does not turn a camera identity hint into a guessed correction");
    fresh=source(snapshot.presentation_request);fresh.camera_manager=a.manager.address;
    new_shot=a;new_shot.world.serial++;
    expect(cg::evaluate_current(snapshot,snapshot.camera,snapshot.time_ns,new_shot,fresh).status==cg::Status::identity_changed,
        "retired world/manager ownership is never reauthorized merely by a similar camera pose");
    snapshot.automatic=false;auto other=a;other.native_camera+=8;
    expect(cg::evaluate(snapshot,snapshot.camera,snapshot.time_ns,&other).dolly==snapshot.dolly,
        "explicit calibration/override offsets are protected from automatic shot-identity handling");
    cg::ViewCache cache;
    expect(!cache.read(1,10,snapshot.camera,snapshot.time_ns), "uninitialized eye decisions are unavailable");
    cache.publish(1,10,snapshot.camera,snapshot.time_ns,{cg::Status::authored_window,700,0});
    const auto left=cache.read(1,10,snapshot.camera,snapshot.time_ns),right=cache.read(1,10,snapshot.camera,snapshot.time_ns+1);
    expect(left && right && left->dolly==right->dolly && left->lift==right->lift && left->status==right->status,
        "both eyes consume one complete framing decision, not independently sampled camera targets");
    auto next_pose=snapshot.camera;next_pose.location[0]+=.01f;
    expect(!cache.read(2,10,snapshot.camera,snapshot.time_ns) && !cache.read(1,11,snapshot.camera,snapshot.time_ns) &&
        !cache.read(1,10,next_pose,snapshot.time_ns) && !cache.read(1,10,snapshot.camera,snapshot.time_ns+250'000'001),
        "eye decisions expire on update generation, engine frame, different view pose and age");
}
}
