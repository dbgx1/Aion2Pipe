#include "jump.hpp"
#include "self_appear_fixture.hpp"
#include "stationary_jump_fixture.hpp"
#include <cmath>
#include <set>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace aion;
static int checks;
static void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"invalid jump accepted");}
static Bytes ground(uint64_t time=10000){
    Bytes frame{34,1,0x37,2};auto put=[&](uint64_t v,int n){for(int i=0;i<n;++i)frame.push_back(uint8_t(v>>(8*i)));};
    for(float v:{1.f,2.f,3.f})put(std::bit_cast<uint32_t>(v),4);
    put(0,2);put(std::bit_cast<uint32_t>(90.f),4);put(1,1);put(time,8);return frame;
}
int main(int argc,char** argv){try{
    JumpTracker entry;check(!entry.worldEntered(),"new connection has no world identity");
    entry.observe(ground(),true,1);check(!entry.worldEntered(),"outgoing motion is not world entry evidence");
    entry.observe(selfAppearFixture(123),false,2);check(entry.worldEntered(),"complete self appearance enables world readiness");
    entry.observe(Bytes{6,0x21,0x36},false,3);check(!entry.worldEntered(),"scene transition invalidates world readiness");
    entry.observe(Bytes{6,0x33,0x36},false,4);check(!entry.worldEntered(),"truncated self appearance cannot enable readiness");
    entry.observe(selfAppearFixture(456),false,5);check(entry.worldEntered(),"new complete appearance restores readiness without timer");
    JumpRequest request{{1,2,3},90,{0,0,1000},10000};
    const Bytes golden{0x2c,2,0x37,0x14,0,0,0x80,0x3f,0,0,0,0x40,0,0,0x40,0x40,
        0,0,0xb4,0x42,0,0,0,0,0,0,0,0,0,0,0x7a,0x44,3,0x10,0x27,0,0,0,0,0,0};
    check(encodeJumpRequest(request)==golden,"wire layout differs from independent golden bytes");
    check(decodeGameFrame(golden,true,true).structureComplete,"jump frame does not fully decode");
    for(auto invalid:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        auto bad=request;bad.position[0]=invalid;rejects([&]{encodeJumpRequest(bad);});
        bad=request;bad.velocity[1]=invalid;rejects([&]{encodeJumpRequest(bad);});
        bad=request;bad.rotation=invalid;rejects([&]{encodeJumpRequest(bad);});
    }
    auto bad=request;bad.velocity[2]=0;rejects([&]{encodeJumpRequest(bad);});bad=request;bad.clientTime=0;rejects([&]{encodeJumpRequest(bad);});
    const auto fixture=stationaryJumpFixture();JumpTrajectory trajectory;
    for(const auto& frame:fixture){auto m=decodeJumpMotion(frame);check(m.has_value(),"real stationary frame decoded");check(encodeJumpMotion(*m)==frame,"native trajectory roundtrip byte exact");trajectory.observe(frame);}
    check(trajectory.ready() && trajectory.size()==10 && trajectory.duration()==900,"complete native stationary jump learned");
    const auto fitOrigin=*decodeJumpMotion(fixture.front());
    for(const auto& bytes:fixture){const auto sample=*decodeJumpMotion(bytes);if(sample.phase!=JumpPhase::Update)continue;
        const double fittedHeight=(double(FlatJumpSpeed)*FlatJumpSpeed-double(sample.velocity[2])*sample.velocity[2])/(2*FlatJumpGravity);
        check(std::abs(sample.position[2]-fitOrigin.position[2]-fittedHeight)<0.005,"flat physics parameters fit independent captured native update heights and velocities");
    }
    auto plan=trajectory.plan({100,200,300},45,100000);
    auto origin=*decodeJumpMotion(fixture.front());
    for(size_t i=0;i<plan.size();++i){auto value=*decodeJumpMotion(plan[i].frame),native=*decodeJumpMotion(fixture[i]);
        check(value.clientTime==100000+plan[i].delayMs && plan[i].delayMs==native.clientTime-origin.clientTime,"native timing preserved with current timebase");
        check(value.position[0]==100 && value.position[1]==200 && std::abs(value.position[2]-(300+native.position[2]-origin.position[2]))<0.01f,"native vertical trajectory rebased");
        check(value.rotation==45 && value.velocity==native.velocity,"current heading and native velocity preserved");
    }
    check(decodeJumpMotion(plan.back().frame)->position==std::array<float,3>{100,200,300},"landing returns exactly to requested ground");
    rejects([&]{trajectory.plan({0,0,0},0,UINT64_MAX-899);});
    rejects([&]{trajectory.plan({NAN,0,0},0,100);});
    auto invalidMotion=*decodeJumpMotion(fixture[0]);invalidMotion.phase=JumpPhase(99);rejects([&]{encodeJumpMotion(invalidMotion);});
    for(size_t cut=0;cut<fixture.size();++cut){JumpTrajectory partial;for(size_t i=0;i<cut;++i)partial.observe(fixture[i]);check(!partial.ready(),"partial sequence cannot enable sending");}
    auto invalidSequence=[&](std::vector<Bytes> frames){JumpTrajectory t;for(auto& f:frames)t.observe(f);check(!t.ready(),"invalid trajectory must not be learned");};
    auto missingFall=fixture;missingFall.erase(missingFall.begin()+5);invalidSequence(missingFall);
    auto moved=fixture;auto move=*decodeJumpMotion(moved[3]);move.position[0]+=1;moved[3]=encodeJumpMotion(move);invalidSequence(moved);
    auto unordered=fixture;std::swap(unordered[2],unordered[3]);invalidSequence(unordered);
    auto unknownStop=fixture;unknownStop.back()[3]=1;invalidSequence(unknownStop);
    auto delayed=fixture;auto late=*decodeJumpMotion(delayed[1]);late.clientTime+=500;delayed[1]=encodeJumpMotion(late);invalidSequence(delayed);
    auto horizontal=fixture;auto drifting=*decodeJumpMotion(horizontal[0]);drifting.velocity[0]=10;horizontal[0]=encodeJumpMotion(drifting);invalidSequence(horizontal);
    JumpTracker tracker;rejects([&]{tracker.prepareSequence(100);});
    tracker.observe(ground(),true,100);tracker.observe(golden,true,200);check(!tracker.state(200).learned,"single start does not provide complete trajectory");
    auto feed=[&](JumpTracker& t,uint64_t start){for(size_t i=0;i<fixture.size();++i)t.observe(fixture[i],true,start+plan[i].delayMs);};
    JumpTracker flat;
    check(flat.state(100).simulated && !flat.state(100).ready,"independent model still needs a current connection baseline");
    flat.observe(ground(),true,100);check(!flat.state(100).ready,"moving is not a stationary baseline");
    const auto nativeLanding=*decodeJumpMotion(fixture.back());
    flat.observe(encodeJumpMotion(nativeLanding),true,200);
    check(flat.state(30200).ready && flat.state(30200).simulated && !flat.state(30200).learned,"ordinary stop enables simulation without any native jump, even after waiting");
    auto independent=flat.prepareSequence(30200);
    check(independent.size()==11 && independent.back().delayMs==888,"independent flat jump includes the full flight and stop");
    size_t falls=0;float peak=0;
    for(const auto& step:independent){const auto m=decodeJumpMotion(step.frame);check(m.has_value(),"generated flat motion is a valid wire layout");
        check(m->clientTime==nativeLanding.clientTime+30000+step.delayMs && m->position[0]==nativeLanding.position[0] && m->position[1]==nativeLanding.position[1] && m->rotation==nativeLanding.rotation,"independent jump uses only current position and monotonic time");
        check(m->velocity[0]==0 && m->velocity[1]==0,"stationary jump has no horizontal velocity");
        const double t=double(step.delayMs)/1000;peak=std::max(peak,m->position[2]-nativeLanding.position[2]);
        if(m->phase!=JumpPhase::Stop){check(std::abs(m->position[2]-nativeLanding.position[2]-(1000*t-1127*t*t))<0.005,"height follows fitted ballistic equation");check(std::abs(m->velocity[2]-(1000-2254*t))<0.001,"vertical velocity follows gravity");}
        else check(m->position==nativeLanding.position,"flat landing returns to the original height exactly");
        if(m->phase==JumpPhase::Fall){++falls;check(m->velocity[2]<0,"fall event follows the apex");}
    }
    check(falls==1 && peak>221 && peak<223,"single fall transition and observed peak height");
    rejects([&]{simulateFlatJump({0,0,0},0,0);});rejects([&]{simulateFlatJump({0,0,0},0,UINT64_MAX-887);});rejects([&]{simulateFlatJump({NAN,0,0},0,1000);});
    flat.submitted(30200);flat.completed(*decodeJumpMotion(independent.back().frame),31088);
    check(flat.state(90000).ready,"stop/restart after idle does not require a manual jump");
    flat.observe(Bytes{6,0x21,0x36},false,90001);check(!flat.state(90002).ready && flat.state(90002).simulated,"scene reset retains only model, never previous position/time");
    feed(tracker,1000);auto current=tracker.prepare(2025);auto nativeStop=*decodeJumpMotion(fixture.back());
    check(current.position==nativeStop.position && current.velocity[2]==1000 && current.clientTime==nativeStop.clientTime+125,"fresh landing position anchors timestamp");
    check(tracker.state(6900).ready && tracker.state(6901).ready && tracker.state(600000).ready,"unchanged stationary baseline no longer expires after five seconds");
    rejects([&]{tracker.prepare(1899);});
    check(tracker.state(20000,true).ready,"explicit unchanged stationary baseline supports long repeat interval");
    auto laterPlan=tracker.prepareSequence(20000,true);check(decodeJumpMotion(laterPlan.front().frame)->clientTime==nativeStop.clientTime+18100,"repeat extrapolates fresh timestamp rather than replaying bytes");
    auto revision=tracker.revision();tracker.observe(Bytes{6,1,0x36},true,2030);check(tracker.revision()==revision,"heartbeat does not interrupt jump");
    tracker.observe(ground(),true,2050);check(tracker.revision()!=revision && !tracker.state(2050).ready,"native movement interrupts and requires new stop");
    check(!tracker.state(2050,true).ready,"stationary reuse cannot bypass real movement");
    tracker.observe(Bytes{6,0x18,0x37},true,2100);check(!tracker.state(2100).ready,"unknown stop cannot enable send");
    feed(tracker,2200);auto sendPlan=tracker.prepareSequence(3200);tracker.submitted(3200);check(!tracker.state(3201).ready,"in-flight sequence cannot be reused");
    tracker.completed(*decodeJumpMotion(sendPlan.back().frame),4100);check(tracker.state(4100).ready,"completed stop restores position baseline");
    check(tracker.state(4100).serverFeedback.find("完整序列已提交")!=std::string::npos,"finished sequence no longer reports sending");
    tracker.observe(Bytes{6,0x21,0x36},false,4101);check(!tracker.state(4101).learned && !tracker.state(4101).ready,"scene reset clears learned trajectory");
    feed(tracker,5000);tracker.observe(Bytes{6,0x3e,0x37},false,6000);check(!tracker.state(6000).ready,"server correction invalidates old ground");
    auto overflowStop=nativeStop;overflowStop.clientTime=UINT64_MAX;tracker.observe(encodeJumpMotion(overflowStop),true,7000);rejects([&]{tracker.prepareSequence(7001);});
    tracker.observe(ground(),true,8000);tracker.observe(Bytes{6,4,0x37},true,8001);check(!tracker.state(8001).ready,"unsupported movement invalidates ground");
    JumpTracker clockTracker;feed(clockTracker,1000);
    const auto anchorTime=decodeJumpMotion(fixture.back())->clientTime;
    for(uint64_t cycle=0;cycle<30;++cycle){
        const uint64_t start=2000+cycle*6000;
        const auto steps=clockTracker.prepareSequence(start,true);
        check(decodeJumpMotion(steps.front().frame)->clientTime==anchorTime+start-1900,"repeated send clock does not accumulate late landing drift");
        clockTracker.submitted(start);
        clockTracker.completed(*decodeJumpMotion(steps.back().frame),start+steps.back().delayMs+80);
    }
    JumpTracker other;check(!other.state(6000).ready,"new connection inherits no trajectory");
    JumpTracker groundNotice;groundNotice.observe(selfAppearFixture(0x3fffffff),false,100);groundNotice.observe(encodeJumpMotion(nativeLanding),true,200);
    Bytes height{11,0x46,0x37,1};
    const auto heightBits=std::bit_cast<uint32_t>(22961.f);for(unsigned i=0;i<4;++i)height.push_back(uint8_t(heightBits>>(8*i)));
    check(decodeGameFrame(height,true).structureComplete,"ground height fixture fully decodes");
    auto unchangedRevision=groundNotice.revision();groundNotice.observe(height,false,250);
    check(groundNotice.revision()==unchangedRevision && groundNotice.state(10000).ready && groundNotice.state(10000).request.position==nativeLanding.position,"ground height is metadata, not a new coordinate or cancellation");
    const auto flatPlan=groundNotice.prepareSequence(10000);groundNotice.submitted(10000);groundNotice.observe(height,false,10121);
    check(groundNotice.revision()==unchangedRevision && !groundNotice.state(10121).ready,"ground height during flight must neither cancel nor mark landing complete");
    groundNotice.completed(*decodeJumpMotion(flatPlan.back().frame),10888);
    Bytes compressedBody{0xff,0xff,8,0,0,0,0x80};compressedBody.insert(compressedBody.end(),height.begin(),height.end());
    compressedBody.insert(compressedBody.begin(),uint8_t(compressedBody.size()+4));groundNotice.observe(compressedBody,false,11000);
    check(groundNotice.revision()==unchangedRevision && groundNotice.state(20000).ready,"compressed ground-height notification preserves next repeat readiness");
    JumpTracker audit;audit.observe(selfAppearFixture(0x3fffffff),false,100);feed(audit,200);audit.submitted(1200);
    // 371A: own key 1, flags=12, Stop, XYZ, yaw, penalty bit=true, duration u64LE 1000.
    Bytes stopBody{0x1a,0x37,1,12,0};stopBody.resize(17,0);stopBody.insert(stopBody.end(),{0,0,1,0xe8,3,0,0,0,0,0,0});
    Bytes stop{uint8_t(stopBody.size()+4)};stop.insert(stop.end(),stopBody.begin(),stopBody.end());
    check(decodeGameFrame(stop,true).structureComplete,"penalty fixture fully decoded");
    auto otherStop=stop;otherStop[3]=2;audit.observe(otherStop,false,1250);check(!audit.state(1250).movementPenaltyObserved,"other entity cannot penalize own jump");
    JumpTracker beforeFirst;beforeFirst.observe(selfAppearFixture(0x3fffffff),false,100);beforeFirst.observe(encodeJumpMotion(nativeLanding),true,200);
    beforeFirst.observe(otherStop,false,300);check(beforeFirst.state(10000).ready,"foreign server movement does not invalidate stationary baseline");
    beforeFirst.observe(stop,false,400);check(!beforeFirst.state(10000).ready && beforeFirst.state(10000).movementPenaltyObserved,"own penalty also blocks simulation before first generated jump");
    audit.observe(stop,false,1300);check(audit.state(1300).movementPenaltyObserved,"own penalty is tracked");
    audit.observe(ground(),true,1400);check(!audit.state(1400).ready,"penalty disables repeated experiments on connection");
    auto evidence=audit.takeEvidence();check(std::any_of(evidence.begin(),evidence.end(),[](const auto& e){return e.first=="jump_self_feedback" && e.second.find("penalty_ms=1000")!=std::string::npos;}),"penalty details retained in audit evidence");
    audit.tick(60400);evidence=audit.takeEvidence();check(std::any_of(evidence.begin(),evidence.end(),[](const auto& e){return e.first=="jump_observation_end";}),"bounded response observation ends");
    audit.ended(61000,"server_eof");check(audit.takeEvidence().back().second.find("server_eof")!=std::string::npos,"EOF side retained without guessing AFK");
    JumpTracker unknown;unknown.submitted(100);unknown.observe(stop,false,200);check(!unknown.state(200).movementPenaltyObserved,"missing self identity cannot attribute another entity");
    unknown.tick(15200);check(unknown.state(15200).serverFeedback.find("未确认")!=std::string::npos,"silence never implies success");
    // Minimal independent 382A fixture: one status, no optional fields.
    Bytes effectBody{0x2a,0x38,1,1,0,1,0xe7,0,0,0,0xc8,0,0,0,0,0,0,0};
    effectBody.resize(26,0);effectBody.insert(effectBody.end(),{1,0});
    Bytes effect{uint8_t(effectBody.size()+4)};effect.insert(effect.end(),effectBody.begin(),effectBody.end());
    check(decodeGameFrame(effect,true).structureComplete,"status fixture complete");
    JumpTracker effects;effects.observe(selfAppearFixture(0x3fffffff),false,100);effects.submitted(200);
    auto foreign=effect;foreign[3]=2;effects.observe(foreign,false,300);check(effects.state(300).serverStateFeedback.empty(),"foreign status excluded");
    auto truncated=effect;truncated.pop_back();effects.observe(truncated,false,350);check(effects.state(350).serverStateFeedback.empty(),"truncated status excluded");
    effects.observe(effect,false,400);check(effects.state(400).serverStateFeedback.find("231")!=std::string::npos,"own status captured separately");
    effects.tick(15300);check(effects.state(15300).serverStateFeedback.find("231")!=std::string::npos && effects.state(15300).serverFeedback.find("未确认")!=std::string::npos,"state feedback persists without becoming a success claim");
    effects.submitted(16000);check(effects.state(16000).serverStateFeedback.empty(),"new attempt clears prior state feedback");
    effects.tick(32000);effects.observe(effect,false,32001);check(effects.state(32001).serverStateFeedback.empty(),"late state excluded");
    effects.observe(stop,false,33000);check(effects.state(33000).movementPenaltyObserved,"late own movement penalty still inhibits repeat");
    if(argc>1){
        bool incomplete=false;auto packets=loadSession(argv[1],&incomplete);std::map<uint64_t,JumpTracker> trackers;size_t submitted=0,penalties=0;std::set<std::string> stateAttempts;
        for(const auto& p:packets){
            if(p.wireObservation || !p.plaintext || p.payload.empty() || p.source.port==13700 || p.destination.port==13700)continue;
            auto& item=trackers[p.proxyConnection];auto now=p.timeUs/1000;
            if(p.toolGenerated){auto motion=decodeJumpMotion(p.payload);if(motion && motion->phase==JumpPhase::Start){item.submitted(now);++submitted;}}
            else item.observe(p.payload,p.outbound,now);
            item.tick(now);for(const auto& e:item.takeEvidence()){
                if(e.first=="jump_self_feedback" && e.second.find("penalty=1")!=std::string::npos)++penalties;
                if(e.first=="jump_self_state_feedback" && e.second.find("status_ids=231 ")!=std::string::npos)stateAttempts.insert(std::to_string(p.proxyConnection)+"/"+e.second.substr(0,e.second.find(' ')));
            }
        }
        std::cout<<"capture starts="<<submitted<<" penalties="<<penalties<<" state attempts="<<stateAttempts.size()<<'\n';
        if(argc>2 && std::string(argv[2])=="repeat-capture")check(submitted==11 && penalties==1 && stateAttempts.size()==11,"repeat capture has eleven state responses and one own penalty across eleven starts");
        else if(argc>2)check(submitted==6 && penalties==0 && stateAttempts.size()==6,"all six complete real jumps expose own state notifications without penalty");
        else check(submitted==3 && penalties==3,"real session must identify all three own movement penalties, including compressed replies");
        std::cout<<"real generated jumps="<<submitted<<" own penalties="<<penalties<<" session_incomplete="<<incomplete<<'\n';
    }
    std::cout<<checks<<" jump checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
