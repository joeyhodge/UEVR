#pragma once

#include "mods/vr/EBaseballFramingRepair.hpp"
#include "mods/vr/ProSpiCameraCutGuard.hpp"
#include "mods/vr/ProSpiNativeCameraLayout.hpp"
#include <limits>
#include <random>

namespace ebaseball_framing_tests {
namespace rp = uevr::prospi::repair;
namespace fg = uevr::prospi::focus_guard;
namespace tr = uevr::prospi::trace;
namespace cg = uevr::prospi::cut_guard;
namespace shot = uevr::prospi::shot;

inline rp::Request request(bool interview = false) {
    rp::Request r{};
    r.camera = {.camera = {{3200,-1400,220},{0,140,0},8,0,0,true},
        .camera_manager = 0x100000, .now_ns = 1'000'000'000, .mode = 3, .zone = 1,
        .enabled = true, .prospi = true, .dolly_enabled = true, .sequencer_enabled = true,
        .safety_enabled = true, .automatic_source = true, .dolly = 1500, .lift = 0,
        .predicted_z = 220, .floor = 170, .max_lift = 400, .base_fov = 90};
    r.field_floor = r.legacy_floor = 170; r.ebaseball = true;
    if (interview) {
        r.camera.camera = {{1300,-420,162},{4,-150,0},31.5f,0,0,true};
        r.camera.dolly = 746;
    }
    return r;
}
inline tr::NativeSource source(const rp::Request& r, float focus = 900) {
    tr::NativeSource s{};
    s.pose = r.camera.camera; s.pose.aspect = 16.0f / 9; s.pose.aspect_valid = true;
    s.object = 0x200000; s.camera_manager = r.camera.camera_manager; s.camera_actor = 0x300000;
    s.time_ns = r.camera.now_ns; s.frame = 40; s.focus_distance = focus; s.status = tr::SourceStatus::accepted;
    const auto f = tr::forward(s.pose);
    for (size_t i = 0; i < 3; ++i) { s.look_at[i] = s.pose.location[i] + f[i] * focus; }
    return s;
}
inline shot::Identity identity() {
    shot::Identity i{};
    i.world = {0x400000,0x500000,1,2}; i.manager = {0x100000,0x500000,3,4};
    i.view_target = {0x600000,0x500000,5,6}; i.native_camera = 0x200000; i.camera_actor = 0x300000;
    i.valid = true; i.cut_known = true;
    return i;
}

template<class Expect> void verify_projection(const rp::Request& r, const tr::NativeSource& native,
    const rp::Decision& d, float margin, Expect&& expect) {
    auto view = r.camera.camera; view.fov = r.camera.base_fov;
    view.aspect = native.pose.aspect; view.aspect_valid = true;
    const auto f = tr::forward(view);
    constexpr float rad = std::numbers::pi_v<float> / 180;
    const auto pitch = view.rotation[0] * rad, yaw = view.rotation[1] * rad, roll = view.rotation[2] * rad;
    const tr::Vector right0{-std::sin(yaw),std::cos(yaw),0};
    const tr::Vector up0{-std::sin(pitch)*std::cos(yaw),-std::sin(pitch)*std::sin(yaw),std::cos(pitch)};
    tr::Vector right{},up{};
    for(size_t i=0;i<3;++i) {
        right[i]=std::cos(roll)*right0[i]-std::sin(roll)*up0[i];
        up[i]=std::sin(roll)*right0[i]+std::cos(roll)*up0[i];
        view.location[i]+=f[i]*d.dolly+up[i]*d.lift;
    }
    const auto width=native.focus_distance*std::tan(native.pose.fov*rad*.5f);
    for(int x : {-1,1}) for(int y : {-1,1}) for(size_t axis=0;axis<3;++axis) for(int sign : {-1,1}) {
        auto corner=native.look_at;
        for(size_t i=0;i<3;++i) { corner[i]+=right[i]*x*width+up[i]*y*width/native.pose.aspect; }
        auto eye=view; eye.location[axis]+=sign*margin;
        const auto projected=tr::target_framing(eye,corner);
        expect(projected.valid && !projected.behind && projected.clip_estimate_valid &&
            std::abs(projected.ndc_x)<=.952f && std::abs(projected.ndc_y)<=.952f,
            "independent rolled corner/eye projections agree with the analytic containment check");
    }
}

template<class Expect> void run(Expect&& expect) {
    for (bool interview : {false, true}) for (bool mirrored : {false, true}) for (float roll : {-20.0f,0.0f,20.0f}) {
        auto r = request(interview);
        if (mirrored) { r.camera.camera.location[0] *= -1; r.camera.camera.rotation[1] = 180 - r.camera.camera.rotation[1]; }
        r.camera.camera.rotation[2] = roll;
        const auto s = source(r, interview ? 267 : 900);
        const auto d = rp::evaluate(r,s,3.3f);
        expect(d.applied && d.dolly >= 0 && d.dolly < r.camera.dolly &&
            rp::window_contains(r,s,d.dolly,d.lift,3.3f),
            "both baseline sides and rolled authored rigs recover a crossed target with the whole stereo window retained");
        expect(d.reason == (interview ? rp::Reason::interview_focus : rp::Reason::dugout_focus),
            "the repair reports its bounded rig family, not a guessed shot ID");
        verify_projection(r,s,d,3.3f,expect);
    }

    auto r = request(); r.camera.dolly = 500; r.camera.lift = 400; r.camera.zone = 3;
    r.camera.floor = r.legacy_floor = 1705;
    auto s = source(r,900); auto d = rp::evaluate(r,s,3.3f);
    expect(d.applied && d.reason == rp::Reason::dugout_height && d.lift < 1 &&
        rp::window_contains(r,s,d.dolly,d.lift,3.3f),
        "a proved offscreen low dugout target can shed the misclassified stand lift without lowering the field floor");
    r.camera.dolly = 100; r.camera.lift = 50;
    s = source(r); d = rp::evaluate(r,s,3.3f);
    expect(!d.applied && d.dolly == r.camera.dolly && d.lift == r.camera.lift,
        "a still-framed low dugout shot is bit-identical even when the legacy floor label looks suspicious");
    r = request(); r.camera.dolly = 850; s = source(r); d = rp::evaluate(r,s,3.3f);
    expect(!d.applied && d.dolly == 850 && d.lift == 0,
        "the authored window alone cannot invalidate a deliberately tight but still-framed zoom");
    r = request(true); r.camera.dolly = 100; r.camera.lift = 200;
    s = source(r,267); d = rp::evaluate(r,s,3.3f);
    expect(d.applied && d.dolly < 100 && rp::window_contains(r,s,d.dolly,d.lift,3.3f),
        "the new interview check also repairs a proven offscreen focus, not just the old crossing case");

    for (int bad = 0; bad < 34; ++bad) {
        r = request(); s = source(r);
        switch (bad) {
        case 0:r.ebaseball=false;break; case 1:r.camera.enabled=false;break;
        case 2:r.manual_controls=true;break; case 3:r.protected_camera=true;break;
        case 4:r.camera.mode=2;break; case 5:r.camera.sequencer_enabled=false;break;
        case 6:r.camera.automatic_source=false;break; case 7:r.camera.safety_enabled=false;break;
        case 8:r.camera.forward_offset=1;break; case 9:r.camera.right_offset=1;break;
        case 10:r.camera.up_offset=1;break; case 11:r.camera.camera.location[2]=8000;break;
        case 12:r.camera.camera.fov=0.5f;break; case 13:r.camera.camera.rotation[2]=26;break;
        case 14:r.field_floor=500;break; case 15:r.camera.max_lift=700;break;
        case 16:s.object=0;break; case 17:s.camera_actor=0;break;
        case 18:s.camera_manager++;break; case 19:s.time_ns-=50'000'001;break;
        case 20:s.pose.location[0]+=2;break; case 21:s.pose.rotation[2]+=1;break;
        case 22:s.pose.fov+=1;break; case 23:s.pose.aspect_valid=false;break;
        case 24:s.look_at[2]+=100;break; case 25:s.focus_distance=1300;break;
        case 26:r.smoothing=std::numeric_limits<float>::quiet_NaN();break;
        case 27:r.camera.protected_camera=true;break;
        case 28:r.camera.camera.rotation[0]=30;break;
        case 29:r.camera.dolly=std::numeric_limits<float>::quiet_NaN();break;
        case 30:r.camera.lift=-1;break; case 31:r.camera.base_fov=20;break;
        case 32:r.camera.zone=4;break;case 33:r.camera.zone=0;break;
        }
        d = rp::evaluate(r,s,3.3f);
        expect(!d.applied && (std::isnan(r.camera.dolly) ? std::isnan(d.dolly) : d.dolly == r.camera.dolly) &&
            d.lift == r.camera.lift, "unsupported/manual/foreign/stale or invalid inputs cannot partially alter a shot");
    }
    r = request(); r.camera.camera.location[2]=-100; r.camera.max_lift=0;
    s = source(r); d = rp::evaluate(r,s,3.3f);
    expect(!d.applied && d.reason == rp::Reason::no_safe_endpoint && d.dolly == r.camera.dolly,
        "an impossible floor/lift budget leaves the existing shot untouched");

    auto i = identity(); r = request(true); s = source(r,267);
    cg::Snapshot snapshot{};
    snapshot.camera = r.camera.camera; snapshot.dolly=r.camera.dolly; snapshot.lift=r.camera.lift;
    snapshot.time_ns=r.camera.now_ns; snapshot.identity=i; snapshot.framing_repair=r;
    rp::Continuity continuity;
    {
        auto retired=i; retired.valid=false;
        const auto declined=continuity.update(r,s,retired,3.3f);
        expect(!declined.applied && declined.dolly==r.camera.dolly && declined.lift==r.camera.lift,
            "the continuity helper itself cannot authorize a repair with an unavailable owner");
        retired=i;retired.native_camera++;
        const auto foreign=continuity.update(r,s,retired,3.3f);
        expect(!foreign.applied && foreign.dolly==r.camera.dolly,
            "a current source from a different native camera cannot acquire numeric continuity");
    }
    auto input=r.camera.camera; input.fov=0; input.aspect_valid=false;
    auto baseline=cg::evaluate_current(snapshot,input,r.camera.now_ns,i,s,3.3f);
    expect(!snapshot.automatic && baseline.dolly==snapshot.dolly,
        "the legacy manual/calibration path itself remains unchanged");
    auto current=cg::repair_current(snapshot,input,r.camera.now_ns,i,s,3.3f,baseline,continuity);
    expect(current.status==cg::Status::authored_window && current.dolly<snapshot.dolly &&
        current.repair_reason==rp::Reason::interview_focus,
        "a selected automatic calibration can be corrected using a fresh native lens even though the offset callback has no FOV");
    for (int bad=0;bad<9;++bad) {
        auto rejected=snapshot; auto owner=i; auto pose=input; auto now=r.camera.now_ns;
        switch(bad) {
        case 0:owner.world.serial++;break;case 1:owner.manager.serial++;break;
        case 2:owner.view_target.serial++;break;case 3:owner.valid=false;break;
        case 4:now+=250'000'001;break;case 5:pose.location[2]+=1000;break;
        case 6:rejected.framing_repair.ebaseball=false;break;
        case 7:owner.view_target={};break;case 8:rejected.framing_repair.camera.camera_manager++;break;
        }
        const auto declined=cg::repair_current(rejected,pose,now,owner,s,3.3f,baseline,continuity);
        expect(declined.dolly==baseline.dolly && declined.lift==baseline.lift &&
            declined.repair_reason==rp::Reason::off,
            "owner/target retirement, unavailable identities, gaps, cuts and 2025 cannot authorize the eBaseball path");
    }

    cg::ViewCache cache;
    cache.publish(1,40,input,r.camera.now_ns,current,true,3.3f);
    expect(cache.read(1,40,input,r.camera.now_ns+1,true,3.3f).has_value(),
        "both eye calls reuse one complete same-frame decision");
    auto changed=input; changed.fov=31;
    expect(!cache.read(1,40,changed,r.camera.now_ns+1,true,3.3f) &&
        !cache.read(1,40,input,r.camera.now_ns+1,true,4) &&
        !cache.read(1,41,input,r.camera.now_ns+1,true,3.3f) &&
        !cache.read(1,40,input,r.camera.now_ns+50'000'001,true,3.3f) &&
        !cache.read(1,40,input,r.camera.now_ns+1),
        "lens, eye displacement, frame, source age and feature changes invalidate the stricter cache");

    const auto recover = [&](int hz) {
        rp::Continuity state;
        auto q=request(); auto native=source(q,900); const auto owner=identity();
        const auto first=state.update(q,native,owner,3.3f);
        float previous=first.dolly;
        for(int tick=1;tick<=hz;++tick) {
            q.camera.now_ns=1'000'000'000+uint64_t(std::llround(double(tick)*1e9/hz));
            native=source(q,1200);
            const auto value=state.update(q,native,owner,3.3f);
            expect(value.dolly>=previous-0.002f && value.dolly<=q.camera.dolly &&
                rp::window_contains(q,native,value.dolly,value.lift,3.3f),
                "recovery only increases toward the new safe bound and revalidates the entire authored window each time");
            if(double(tick)/hz < .25) { expect(value.dolly==first.dolly, "short source fluctuations do not re-arm an overshooting dolly"); }
            previous=value.dolly;
        }
        return previous;
    };
    const auto at30=recover(30),at60=recover(60),at120=recover(120);
    expect(std::abs(at30-at60)<0.01f && std::abs(at30-at120)<0.01f,
        "recovery uses the existing 30-Hz reference response consistently at 30/60/120 updates");
    {
        rp::Continuity state; r=request(); s=source(r);
        state.update(r,s,i,3.3f);
        r.camera.now_ns+=33'333'333; r.camera.dolly=850; s=source(r);
        const auto value=state.update(r,s,i,20);
        expect(value.reason==rp::Reason::unchanged && value.dolly==r.camera.dolly,
            "a currently good shot is bit-identical even after a previous repair, not subject to a recovery hold");
        const auto held=rp::geometry(r,r.camera.camera,r.camera.now_ns,850,0);
        const auto point=fg::authored_endpoint_visible(held,s,rp::scope(r));
        expect(point && *point && !rp::window_contains(r,s,850,0,3.3f),
            "a visible target point alone cannot authorize a hold that crops its authored window");
    }
    for(int reset=0;reset<6;++reset) {
        rp::Continuity state; r=request(); s=source(r); const auto first=state.update(r,s,i,3.3f);
        r.camera.now_ns+=33'333'333; auto owner=i;
        switch(reset) {
        case 0:owner.view_target.serial++;break;case 1:owner.cut=true;break;
        case 2:r.rendering_method++;break;case 3:r.camera.now_ns+=300'000'000;break;
        case 4:r.camera.camera.rotation[1]+=40;break;case 5:state.reset();break;
        }
        s=source(r,1200); const auto value=state.update(r,s,owner,3.3f);
        expect(value.dolly>first.dolly && value.reason!=rp::Reason::release_hold,
            "cuts, generation changes, rendering-mode switches, gaps and disable reset the numeric hold");
    }

    std::mt19937 random(0x20261107);
    const auto value=[&](float low,float high) { return std::uniform_real_distribution<float>(low,high)(random); };
    unsigned repaired{},preserved{};
    for(unsigned trial=0;trial<4000;++trial) {
        r=request(trial%2!=0);
        r.camera.camera.rotation[0]=value(-4,10); r.camera.camera.rotation[2]=value(-24,24);
        const auto focus=value(150,rp::scope(r)==fg::AuthoredScope::interview_repair ? 950.0f:1200.0f);
        r.camera.dolly=focus*value(.05f,1.6f); r.camera.lift=value(0,400);
        s=source(r,focus); const auto margin=value(0,8); d=rp::evaluate(r,s,margin);
        if(d.applied) {
            ++repaired;
            expect(d.dolly<r.camera.dolly && rp::window_contains(r,s,d.dolly,d.lift,margin),
                "randomized rolled repairs satisfy the full endpoint and eye-margin contract");
            verify_projection(r,s,d,margin,expect);
        } else {
            ++preserved;
            expect(d.dolly==r.camera.dolly && d.lift==r.camera.lift,
                "a rejected or already-good randomized shot is an exact no-op");
        }
    }
    expect(repaired>500 && preserved>500, "randomized coverage exercises both repairs and unchanged good shots");
    for(size_t build=0;build<3;++build) {
        auto layout=tr::native::verified_builds[build].layout; layout.base=0x140000000;
        expect(tr::native::ebaseball_layout(layout)==(build<2), "both validated eBaseball builds are isolated from ProSpi 2025");
        layout.bridge_vtable_rva++;
        expect(!tr::native::ebaseball_layout(layout), "partial or foreign layout contracts cannot pass the variant gate");
    }
}
}
