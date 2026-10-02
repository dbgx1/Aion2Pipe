#include "world_probe.hpp"
#include <algorithm>
namespace aion {
namespace {
struct Header {size_t length{}, prefix{}; uint16_t opcode{};};
std::optional<Header> header(std::span<const uint8_t> b){
    uint32_t n=0;
    for(size_t i=0;i<4 && i<b.size();++i){
        n|=uint32_t(b[i]&127)<<(7*i);
        if(b[i]&128)continue;
        if(n<6 || n>65536 || b.size()<i+3)return {};
        return Header{size_t(n-4+i+1),i+1,uint16_t(b[i+1]|(uint16_t(b[i+2])<<8))};
    }
    return {};
}
bool candidate(uint16_t op){
    switch(op){
    case 0x3633:case 0x3634:case 0x3635:case 0x3641:case 0x3642:case 0x3645:case 0x3647:
    case 0x371A:case 0x371B:case 0x371C:case 0x371D:case 0x3729:case 0x372A:
    case 0x8D00:case 0x382A:case 0x382B:case 0x382C:case 0x8A33:case 0xFFFF:return true;
    default:return false;
    }
}
bool verify(std::span<const uint8_t> b,size_t& count,size_t& anchors,size_t& budget,unsigned depth=0){
    auto h=header(b);if(!h || h->length!=b.size() || h->opcode<0x3600 || !budget || depth>2)return false;
    --budget;
    // Bound decompression work before invoking the general-purpose decoder.
    if(h->opcode==0xFFFF && (b.size()<h->prefix+6 || readInteger(b,h->prefix+2,4,false)>262144))return false;
    auto m=decodeGameFrame(b,true);if(!m.structureComplete)return false;
    if(h->opcode!=0xFFFF){++count;if(candidate(h->opcode))++anchors;return true;}
    auto frames=splitGameFrames(m.expanded);if(frames.consumed!=m.expanded.size() || frames.frames.empty())return false;
    for(const auto& f:frames.frames){
        if(!verify(std::span(m.expanded).subspan(f.offset,f.length),count,anchors,budget,depth+1))return false;
        if(count>=3 && anchors)return true;
    }
    return true;
}
}
std::optional<size_t> findWorldStreamStart(std::span<const uint8_t> bytes){
    size_t budget=128;
    // Search a bounded initial window, including mid-frame capture starts. Never
    // identify a connection from a port or one coincidental opcode alone.
    for(size_t start=0;start<std::min<size_t>(bytes.size(),65536) && budget;++start){
        size_t pos=start,count=0,anchors=0;
        for(size_t outer=0;outer<3 && pos<bytes.size();++outer){
            auto h=header(bytes.subspan(pos));
            if(!h || (!outer && !candidate(h->opcode)) || h->length>bytes.size()-pos)break;
            if(!verify(bytes.subspan(pos,h->length),count,anchors,budget))break;
            if(count>=3 && anchors){
                // Preserve earlier handshake/appearance frames when framing from
                // byte zero reaches the independently verified anchor exactly.
                // A mid-frame prefix must not be treated as a valid boundary.
                size_t earlier=0;
                while(earlier<start){
                    auto preceding=header(bytes.subspan(earlier));
                    if(!preceding || preceding->length>start-earlier)break;
                    earlier+=preceding->length;
                }
                return earlier==start?0:start;
            }
            pos+=h->length;
        }
    }
    return {};
}
}
