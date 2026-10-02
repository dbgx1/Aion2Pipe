#include "query_stream.hpp"
#include <algorithm>
#include <stdexcept>
namespace aion {
QueryStream::QueryStream(Endpoint s,Endpoint d,uint32_t seq):source_(s),destination_(d),sequence_(seq){}
Bytes QueryStream::feed(std::span<const uint8_t> input){
    observations.clear();
    if(!traceable_)return Bytes(input.begin(),input.end());
    if(input.size()>2*1024*1024 || pending_.size()+input.size()>4*1024*1024)throw std::runtime_error("发送帧超过限制");
    pending_.insert(pending_.end(),input.begin(),input.end());
    auto split=splitGameFrames(pending_);
    if(split.status.starts_with("无效") || split.status.starts_with("帧超过")){
        if(client_)throw std::runtime_error("发送流帧边界无效");
        traceable_=false;pending_.clear();history_.clear();
        return Bytes(input.begin(),input.end());
    }
    Bytes out;out.reserve(split.consumed);
    for(const auto& f:split.frames){
        auto original=std::span(pending_).subspan(f.offset,f.length);
        if(!client_){
            // Keep a bounded, frame-aligned history for importing a recent anchor.
            constexpr size_t limit=8*1024*1024;
            if(history_.size()+original.size()>limit){sequence_+=uint32_t(history_.size());history_.clear();}
            history_.insert(history_.end(),original.begin(),original.end());
        }else{
            Bytes frame(original.begin(),original.end());auto body=std::span(frame).subspan(f.prefixBytes);
            client_->transform(body);
            observations.push_back({true,true,false,Bytes(original.begin(),original.end()),frame});
            if(confirmed_<3 && decodeGameFrame(frame,true,true).structureComplete)++confirmed_;
            if(body.size()>=2 && readInteger(body,0,2,false)==0x364f)nativeQuery_=true;
            server_->transform(body);out.insert(out.end(),frame.begin(),frame.end());
        }
    }
    consumed_+=split.consumed;
    pending_.erase(pending_.begin(),pending_.begin()+split.consumed);
    // Before arming, even partial or unknown handshake bytes pass immediately.
    return client_?out:Bytes(input.begin(),input.end());
}
void QueryStream::arm(const CipherSnapshot& snapshot){
    if(!traceable_)throw std::runtime_error("该连接发送边界未识别，仅支持原样转发");
    if(client_)throw std::runtime_error("该连接已导入状态，不能替换正在使用的密码状态");
    if(!boundary())throw std::runtime_error("等待完整发送帧后再导入状态");
    if(snapshot.source!=source_ || snapshot.destination!=destination_)throw std::runtime_error("状态不属于这条连接");
    auto offset=int32_t(snapshot.frameSequence-sequence_);
    if(offset<0 || size_t(offset)>=history_.size())throw std::runtime_error("当前代理历史未包含会话锚点");
    // Anchor must coincide with a known original frame boundary, not arbitrary bytes.
    auto all=splitGameFrames(history_);bool found=false;
    for(const auto& f:all.frames)found|=f.offset==size_t(offset);
    if(!found)throw std::runtime_error("会话锚点不在发送帧起点");
    auto tail=std::span(history_).subspan(size_t(offset));auto split=splitGameFrames(tail);
    auto state=snapshot;size_t confirmed=0;bool native=false;
    for(const auto& f:split.frames){
        Bytes plain(tail.begin()+f.offset,tail.begin()+f.offset+f.length);
        state.transform(std::span(plain).subspan(f.prefixBytes));
        auto decoded=decodeGameFrame(plain,true,true);
        confirmed+=decoded.structureComplete;
        if(plain.size()>=f.prefixBytes+2 && readInteger(plain,f.prefixBytes,2,false)==0x364f)native=true;
    }
    // Structural validation catches stale/mismatched state; it is not a MAC.
    if(confirmed<3)throw std::runtime_error("未验证到至少三条完整出站消息，拒绝启用发送");
    client_=server_=state;nativeQuery_=native;confirmed_=confirmed;history_.clear();
}
void QueryStream::initialize(std::span<const uint8_t> key){
    if(client_ || !boundary() || key.empty() || key.size()>214)throw std::runtime_error("握手密码状态无法初始化");
    CipherSnapshot state;state.source=source_;state.destination=destination_;state.i=1;
    for(unsigned i=0;i<256;++i)state.table[i]=uint8_t(i);
    uint8_t j=0;for(unsigned i=0;i<256;++i){j=uint8_t(j+state.table[i]+key[i%key.size()]);std::swap(state.table[i],state.table[j]);}
    client_=server_=state;confirmed_=0;history_.clear();
}
Bytes QueryStream::guild(bool search,uint8_t order,std::string_view name){
    if(!ready() || !verified() || !boundary())throw std::runtime_error("军团查询需要已验证的完整帧边界");
    auto wire=encodeGuildRequest(search,order,name);
    auto frames=splitGameFrames(wire);
    server_->transform(std::span(wire).subspan(frames.frames.front().prefixBytes));shifted_=true;return wire;
}
Bytes QueryStream::query(uint32_t server,uint64_t dbid){
    if(!ready() || !boundary())throw std::runtime_error("发送流尚未同步到完整帧边界");
    auto wire=encodeViewCharRequest(server,dbid);
    server_->transform(std::span(wire).subspan(1));shifted_=true;return wire;
}
Bytes QueryStream::jump(const JumpRequest& request){
    if(!ready() || !verified() || !boundary())throw std::runtime_error("跳跃需要已验证的连接及完整发送帧边界");
    auto wire=encodeJumpRequest(request); // Validate before advancing the cipher.
    server_->transform(std::span(wire).subspan(1));shifted_=true;return wire;
}
Bytes QueryStream::jumpMotion(std::span<const uint8_t> frame){
    if(!ready() || !verified() || !boundary())throw std::runtime_error("跳跃序列需要已验证的完整发送边界");
    if(!decodeJumpMotion(frame))throw std::invalid_argument("不支持的跳跃运动帧");
    Bytes wire(frame.begin(),frame.end());server_->transform(std::span(wire).subspan(1));shifted_=true;return wire;
}
}
