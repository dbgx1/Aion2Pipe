#include "jump.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <charconv>
namespace aion {
Bytes encodeJumpRequest(const JumpRequest& request){
    for(auto v:request.position)if(!std::isfinite(v))throw std::invalid_argument("跳跃位置无效");
    for(auto v:request.velocity)if(!std::isfinite(v))throw std::invalid_argument("跳跃速度无效");
    if(!std::isfinite(request.rotation) || request.velocity[2]<=0 || !request.clientTime)
        throw std::invalid_argument("起跳方向、竖直速度或客户端时间无效");
    // Native 0x92C5B00: world coordinates, jump_up + dead_reckoning.
    // Observed 41-byte frame: length ULEB=44, opcode LE, optional flags=0x14.
    Bytes frame{44,2,0x37,0x14};
    auto put=[&](uint64_t v,size_t n){for(size_t i=0;i<n;++i)frame.push_back(uint8_t(v>>(8*i)));};
    for(auto v:request.position)put(std::bit_cast<uint32_t>(v),4);
    put(std::bit_cast<uint32_t>(request.rotation),4);
    for(auto v:request.velocity)put(std::bit_cast<uint32_t>(v),4);
    frame.push_back(3); // Shared bool byte: jump_up bit 0, dead_reckoning bit 1.
    put(request.clientTime,8);
    return frame;
}
void JumpTracker::record(std::string event,std::string details){
    if(evidence_.size()<64)evidence_.emplace_back(std::move(event),"attempt="+std::to_string(attempts_)+" "+std::move(details));
}
void JumpTracker::submitted(uint64_t now){
    stateFeedback_.clear();grounded_=false;++attempts_;submittedAt_=lastSummaryAt_=now;windowClosed_=false;
    feedback_="跳跃序列发送中；等待自身服务器反馈";
    record("jump_observation_begin","self="+(selfKey_?std::to_string(*selfKey_):"unknown"));
}
void JumpTracker::tick(uint64_t now){
    if(!attempts_)return;
    if(!windowClosed_ && now-submittedAt_>15000){windowClosed_=true;record("jump_observation_end","elapsed_ms="+std::to_string(now-submittedAt_)+" penalty_observed="+std::to_string(penaltyObserved_));
        if(feedback_.starts_with("跳跃序列") || feedback_.starts_with("完整序列已提交"))feedback_="15 秒内未识别到自身运动反馈；服务器结果未确认";
    }
}
void JumpTracker::ended(uint64_t now,const char* side){
    if(attempts_)record("jump_connection_end","side="+std::string(side)+" since_jump_ms="+std::to_string(now-submittedAt_)+" since_native_move_ms="+(lastNaturalMoveAt_?std::to_string(now-lastNaturalMoveAt_):"unknown")+" note=disconnect_reason_unconfirmed");
}
void JumpTracker::inbound(std::span<const uint8_t> frame,unsigned depth,size_t& budget,uint64_t now){
    auto split=splitGameFrames(frame);
    for(auto f:split.frames){auto part=frame.subspan(f.offset,f.length);auto op=readInteger(part,f.prefixBytes,2,false);
        if(op==0x3621 || op==0x3633){grounded_=false;jumpSpeed_=0;selfKey_.reset();trajectory_.reset();nativeClockTime_=nativeClockAt_=0;++revision_;
            if(attempts_){windowClosed_=true;record("jump_observation_scene_change","opcode="+std::to_string(op));}}
        if(op==0x373e || op==0x373f || op==0x3740){grounded_=false;++revision_;}
        const bool movement=(op>=0x371a && op<=0x3720) || op==0x3729 || op==0x372a || op==0x372e || op==0x372f || op==0x3746 || (op>=0x373e && op<=0x3740);
        if(op==0x3633 || movement || (op==0x382a && attempts_ && !windowClosed_ && now-submittedAt_<=15000)){
            auto m=decodeGameFrame(part,true);std::optional<uint64_t> entity;bool penalty=false;std::string duration,reason,statusIds;
            for(const auto& field:m.fields){
                if(field.name=="实体编号"){uint64_t value=0;auto [end,error]=std::from_chars(field.value.data(),field.value.data()+field.value.size(),value);if(error==std::errc{} && end==field.value.data()+field.value.size())entity=value;}
                if(field.name.ends_with(" 异常状态 ID") && statusIds.size()<100){if(!statusIds.empty())statusIds+=",";statusIds+=field.value;}
                if(field.name=="移动惩罚启用")penalty=field.value=="true";
                if(field.name=="移动惩罚时长（毫秒）")duration=field.value;
                if(field.name=="拒绝原因（枚举待确认）")reason=field.value;
            }
            if(op==0x3633 && m.structureComplete)selfKey_=entity;
            else if(op==0x382a && m.structureComplete && entity && selfKey_ && entity==selfKey_ && !statusIds.empty()){
                stateFeedback_="观察窗口内收到自身状态通知（ID "+statusIds+"）；与跳跃的关联尚未确认";
                record("jump_self_state_feedback","elapsed_ms="+std::to_string(now-submittedAt_)+" opcode=14378 entity="+std::to_string(*entity)+" status_ids="+statusIds+" note=correlation_not_acceptance");
            }
            else if(movement && entity && selfKey_ && entity==selfKey_){
                // 3746 reports the last ground height (also used during flight).
                // It neither changes the current position nor orders a stop.
                // Cancelling here would leave a generated jump without landing.
                if(op!=0x3746 || !m.structureComplete){grounded_=stopped_=false;++revision_;}
                record("jump_self_feedback","elapsed_ms="+std::to_string(now-submittedAt_)+" opcode="+std::to_string(op)+" entity="+std::to_string(*entity)+" complete="+std::to_string(m.structureComplete)+" penalty="+std::to_string(penalty)+" penalty_ms="+duration+" reason="+reason);
                if(m.structureComplete && penalty){penaltyObserved_=true;++revision_;feedback_="收到自身移动惩罚（"+duration+" 毫秒）；本连接暂停实验发送。原因尚未确认";}
                else if(!penaltyObserved_ && !windowClosed_)feedback_=(op>=0x373e && op<=0x3740)?"收到自身移动拒绝/校正；不能判定跳跃成功":"收到自身运动/地面高度反馈；尚非跳跃成功确认";
            }
        }
        if(op==0xffff){
            if(depth>=4){grounded_=false;continue;}
            auto m=decodeGameFrame(part,true);
            if(m.expanded.empty() || m.expanded.size()>budget){grounded_=false;continue;}
            budget-=m.expanded.size();inbound(m.expanded,depth+1,budget,now);
        }
    }
}
void JumpTracker::observe(std::span<const uint8_t> frame,bool outbound,uint64_t now){
    if(!outbound){lastServerAt_=now;size_t budget=8*1024*1024;inbound(frame,0,budget,now);return;}
    auto split=splitGameFrames(frame);
    if(split.frames.size()!=1 || split.consumed!=frame.size())return;
    auto op=readInteger(frame,split.frames[0].prefixBytes,2,false);
    trajectory_.observe(frame);
    if(op>=0x3700 && op<=0x3718){lastNaturalMoveAt_=now;++revision_;
        if(attempts_ && !windowClosed_ && now-submittedAt_<=15000)record("jump_native_movement_during_observation","elapsed_ms="+std::to_string(now-submittedAt_)+" opcode="+std::to_string(op));}
    if(op==0x3718){
        if(auto stop=decodeJumpMotion(frame);stop && stop->phase==JumpPhase::Stop){
            ground_.position=stop->position;ground_.rotation=stop->rotation;ground_.clientTime=stop->clientTime;
            grounded_=stopped_=true;observedAt_=now;nativeClockTime_=stop->clientTime;nativeClockAt_=now;
        }else {grounded_=false;stopped_=false;}
        return;
    }
    if(op<0x3700 || op>0x3718)return;
    if(op>0x3703){grounded_=false;return;}
    auto m=decodeGameFrame(frame,true,true);
    if(!m.structureComplete){grounded_=false;return;}
    auto field=[&](const char* name)->const GameField*{for(const auto& f:m.fields)if(f.name==name)return &f;return nullptr;};
    auto real=[&](const char* name){const auto* f=field(name);return f&&f->size==4?std::bit_cast<float>(uint32_t(readInteger(frame,f->offset,4,false))):std::numeric_limits<float>::quiet_NaN();};
    if(op>=0x3702){
        grounded_=false;
        auto flags=field("可选字段位图"),jump=field("是否向上跳跃");auto vz=real("移动速度 Z");
        if(op==0x3702 && flags && flags->value=="20" && jump && jump->value=="true" && std::isfinite(vz) && vz>0)jumpSpeed_=vz;
        return;
    }
    JumpRequest value;
    value.position={real("X"),real("Y"),real("Z")};value.rotation=real("朝向（单位待确认）");
    auto timestamp=field("客户端时间（时基待确认）");if(!timestamp){grounded_=false;return;}
    value.clientTime=readInteger(frame,timestamp->offset,8,false);value.velocity={0,0,1};
    try{encodeJumpRequest(value);}catch(...){grounded_=false;return;}
    ground_=value;observedAt_=now;nativeClockTime_=value.clientTime;nativeClockAt_=now;grounded_=true;stopped_=false;
}
JumpState JumpTracker::state(uint64_t now,bool /*stationaryReuse*/)const{
    JumpState out;out.learned=trajectory_.ready();out.grounded=grounded_;out.stopPositionUnconfirmed=false;
    out.simulated=!out.learned;
    out.trajectoryFrames=out.simulated?FlatJumpFrames:trajectory_.size();out.trajectoryDurationMs=out.simulated?FlatJumpDurationMs:trajectory_.duration();
    out.movementPenaltyObserved=penaltyObserved_;out.serverFeedback=feedback_;out.serverStateFeedback=stateFeedback_;
    out.ageMs=now>=observedAt_?now-observedAt_:UINT64_MAX;out.request=ground_;out.request.velocity={0,0,out.simulated?FlatJumpSpeed:trajectory_.speed()};
    const auto clockAge=now>=nativeClockAt_?now-nativeClockAt_:UINT64_MAX;
    if(penaltyObserved_)out.status="本连接已观察到自身移动惩罚，暂停实验发送";
    else if(!grounded_ || !stopped_)out.status="等待本次连接的停止位置；在平地走一步停下即可，无需跳跃";
    else if(out.ageMs==UINT64_MAX)out.status="位置时间无效，等待本次连接的新停止上报";
    else if(!nativeClockTime_ || clockAge>UINT64_MAX-out.trajectoryDurationMs || nativeClockTime_>UINT64_MAX-clockAge-out.trajectoryDurationMs)out.status="客户端时间超出可用范围";
    else {out.ready=true;out.request.clientTime=nativeClockTime_+clockAge;out.status=out.simulated?"可独立模拟平地原地跳跃，无需先手动跳跃":"可发送完整原地跳跃序列";}
    return out;
}
JumpRequest JumpTracker::prepare(uint64_t now)const{
    auto current=state(now);if(!current.ready)throw std::runtime_error(current.status);
    return current.request;
}
std::vector<JumpStep> JumpTracker::prepareSequence(uint64_t now,bool stationaryReuse)const{
    const auto current=state(now,stationaryReuse);if(!current.ready)throw std::runtime_error(current.status);
    return current.simulated?simulateFlatJump(current.request.position,current.request.rotation,current.request.clientTime):trajectory_.plan(current.request.position,current.request.rotation,current.request.clientTime);
}
void JumpTracker::completed(const JumpMotion& stop,uint64_t now){
    if(stop.phase!=JumpPhase::Stop || penaltyObserved_)return;
    ground_.position=stop.position;ground_.rotation=stop.rotation;ground_.clientTime=stop.clientTime;
    grounded_=stopped_=true;observedAt_=now;
    if(feedback_.starts_with("跳跃序列"))feedback_="完整序列已提交；等待自身服务器反馈";
    record("jump_sequence_finished","note=all_frames_submitted_server_result_unconfirmed");
}
}
