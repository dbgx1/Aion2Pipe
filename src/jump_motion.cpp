#include "jump_motion.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
namespace aion {
namespace {
bool finite(const JumpMotion& m){return m.clientTime && std::isfinite(m.rotation) &&
    std::all_of(m.position.begin(),m.position.end(),[](float v){return std::isfinite(v);}) &&
    std::all_of(m.velocity.begin(),m.velocity.end(),[](float v){return std::isfinite(v);});}
}
std::optional<JumpMotion> decodeJumpMotion(std::span<const uint8_t> b){
    if(b.size()<5 || b[0]!=b.size()+3)return {};
    JumpMotion m;size_t at=4;auto op=readInteger(b,1,2,false);
    if(op==0x3702 && b.size()==41 && b[3]==0x14 && b[32]==3)m.phase=JumpPhase::Start;
    else if(op==0x3702 && b.size()==42 && b[3]==0x11 && b[4]==2 && b[33]==1){m.phase=JumpPhase::Fall;at=5;}
    else if(op==0x3703 && b.size()==41 && b[3]==2 && b[32]==1)m.phase=JumpPhase::Update;
    else if(op==0x3718 && b.size()==29 && b[3]==0 && b[4]==0){m.phase=JumpPhase::Stop;at=5;}
    else return {};
    auto real=[&](){auto v=std::bit_cast<float>(uint32_t(readInteger(b,at,4,false)));at+=4;return v;};
    for(auto& v:m.position)v=real();m.rotation=real();
    if(m.phase!=JumpPhase::Stop){for(auto& v:m.velocity)v=real();++at;}
    m.clientTime=readInteger(b,at,8,false);
    if(!finite(m) || (m.phase==JumpPhase::Start && m.velocity[2]<=0) || (m.phase==JumpPhase::Fall && m.velocity[2]>=0))return {};
    return m;
}
Bytes encodeJumpMotion(const JumpMotion& m){
    if(!finite(m))throw std::invalid_argument("跳跃轨迹参数无效");
    Bytes b;
    switch(m.phase){
    case JumpPhase::Start:if(m.velocity[2]<=0)throw std::invalid_argument("起跳竖直速度无效");b={44,2,0x37,0x14};break;
    case JumpPhase::Update:b={44,3,0x37,2};break;
    case JumpPhase::Fall:if(m.velocity[2]>=0)throw std::invalid_argument("下落竖直速度无效");b={45,2,0x37,0x11,2};break;
    case JumpPhase::Stop:b={32,0x18,0x37,0,0};break;
    default:throw std::invalid_argument("未知跳跃阶段");
    }
    auto put=[&](uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));};
    for(auto v:m.position)put(std::bit_cast<uint32_t>(v),4);put(std::bit_cast<uint32_t>(m.rotation),4);
    if(m.phase!=JumpPhase::Stop){for(auto v:m.velocity)put(std::bit_cast<uint32_t>(v),4);b.push_back(m.phase==JumpPhase::Start?3:1);}
    put(m.clientTime,8);return b;
}
std::vector<JumpStep> simulateFlatJump(std::array<float,3> position,float rotation,uint64_t clientTime){
    if(!clientTime || clientTime>UINT64_MAX-FlatJumpDurationMs)throw std::invalid_argument("平地跳跃时间无效");
    std::vector<JumpStep> plan;plan.reserve(FlatJumpFrames);
    for(uint64_t delay:{0ULL,100ULL,200ULL,300ULL,400ULL,FlatJumpFallMs,500ULL,600ULL,700ULL,800ULL,FlatJumpDurationMs}){
        JumpMotion sample;sample.position=position;sample.rotation=rotation;sample.clientTime=clientTime+delay;
        sample.phase=delay==0?JumpPhase::Start:delay==FlatJumpDurationMs?JumpPhase::Stop:delay==FlatJumpFallMs?JumpPhase::Fall:JumpPhase::Update;
        if(sample.phase!=JumpPhase::Stop){const double t=double(delay)/1000.;
            sample.position[2]+=float(FlatJumpSpeed*t-0.5*FlatJumpGravity*t*t);
            sample.velocity[2]=float(FlatJumpSpeed-FlatJumpGravity*t);
        }
        plan.push_back({delay,encodeJumpMotion(sample)});
    }
    return plan;
}
void JumpTrajectory::observe(std::span<const uint8_t> frame){
    auto parsed=decodeJumpMotion(frame);
    if(!parsed){auto frames=splitGameFrames(frame);if(frames.frames.size()==1){auto op=readInteger(frame,frames.frames[0].prefixBytes,2,false);if(op>=0x3700 && op<=0x3718)candidate_.clear();}return;}
    const auto& m=*parsed;
    if(m.phase==JumpPhase::Start){candidate_.clear();if(std::abs(m.velocity[0])<=0.01f && std::abs(m.velocity[1])<=0.01f)candidate_.push_back(m);return;}
    if(candidate_.empty())return;
    const auto first=candidate_.front(),previous=candidate_.back();
    if(candidate_.size()>=64 || m.clientTime<=previous.clientTime || m.clientTime-first.clientTime>3000 || m.clientTime-previous.clientTime>300 ||
       std::abs(m.position[0]-first.position[0])>0.05f || std::abs(m.position[1]-first.position[1])>0.05f ||
       std::abs(m.rotation-first.rotation)>0.05f || std::abs(m.velocity[0])>0.01f || std::abs(m.velocity[1])>0.01f || m.position[2]<first.position[2]-0.1f){candidate_.clear();return;}
    candidate_.push_back(m);
    if(m.phase!=JumpPhase::Stop)return;
    const auto falls=std::count_if(candidate_.begin(),candidate_.end(),[](const auto& p){return p.phase==JumpPhase::Fall;});
    const auto updates=std::count_if(candidate_.begin(),candidate_.end(),[](const auto& p){return p.phase==JumpPhase::Update;});
    const bool rise=std::any_of(candidate_.begin(),candidate_.end(),[&](const auto& p){return p.position[2]>first.position[2]+0.1f;});
    const auto fall=std::find_if(candidate_.begin(),candidate_.end(),[](const auto& p){return p.phase==JumpPhase::Fall;});
    const bool ascent=std::any_of(candidate_.begin(),fall,[](const auto& p){return p.phase==JumpPhase::Update && p.velocity[2]>0;});
    const bool descent=fall!=candidate_.end() && std::any_of(fall,candidate_.end(),[](const auto& p){return p.phase==JumpPhase::Update && p.velocity[2]<0;});
    if(falls==1 && updates>=2 && ascent && descent && rise && m.clientTime-first.clientTime>=500 && std::abs(m.position[2]-first.position[2])<=0.1f)learned_=candidate_;
    candidate_.clear();
}
std::vector<JumpStep> JumpTrajectory::plan(std::array<float,3> position,float rotation,uint64_t time)const{
    if(!ready() || !time || time>UINT64_MAX-duration())throw std::runtime_error("尚无完整原地跳跃轨迹或时间无效");
    std::vector<JumpStep> out;out.reserve(learned_.size());
    for(auto sample:learned_){auto delay=sample.clientTime-learned_.front().clientTime;
        for(size_t i=0;i<3;++i)sample.position[i]=position[i]+(sample.position[i]-learned_.front().position[i]);
        if(sample.phase==JumpPhase::Stop)sample.position=position;
        sample.rotation=rotation;sample.clientTime=time+delay;out.push_back({delay,encodeJumpMotion(sample)});
    }return out;
}
}
