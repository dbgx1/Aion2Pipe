#include "nearby.hpp"
#include "self_appear_fixture.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace aion;
static size_t checks{};
static void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static void put(Bytes& b,uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));}
static Bytes frame(Bytes b){Bytes out;auto n=uint32_t(b.size()+4);do{auto c=uint8_t(n&127);n>>=7;out.push_back(c|(n?128:0));}while(n);out.insert(out.end(),b.begin(),b.end());return out;}
static Bytes body(uint16_t op,uint8_t flags,float x=10){
    Bytes b;put(b,op,2);b.push_back(flags);
    if(op==0x3702 && (flags&1))b.push_back(1);
    for(auto f:{x,20.f,30.f})put(b,std::bit_cast<uint32_t>(f),4);
    if(op<0x3702)put(b,32768,2);
    put(b,std::bit_cast<uint32_t>(-15.f),4);
    if(op>=0x3702)for(auto f:{100.f,-200.f,300.f})put(b,std::bit_cast<uint32_t>(f),4);
    // Non-adjacent booleans share one bit cursor; the springboard is between calls.
    if(op==0x3702){if(flags&4)b.push_back(3);if(flags&8)put(b,0x100000001,8);if((flags&16) && !(flags&4))b.push_back(1);}
    else if(flags&2)b.push_back(1);
    put(b,1790672818300,8);return b;
}
static Packet packet(const CipherSnapshot& s,uint32_t seq,Bytes b){Packet p;p.protocol=6;p.sequence=seq;p.source=s.source;p.destination=s.destination;p.payload=std::move(b);return p;}
int main(int argc,char** argv){try{
    for(auto op:{0x3700,0x3701,0x3702,0x3703})for(uint8_t flags=0;flags<(op==0x3702?32:4);++flags){
        if(flags&(op==0x3702?2:1))continue;
        auto b=body(uint16_t(op),flags);auto plain=frame(b);auto m=decodeGameFrame(plain,true,true);
        check(m.structureComplete,"all confirmed optional scalar layouts consume exactly");
        check(!decodeGameFrame(plain,true,false).structureComplete,"TX movement schema is direction-specific");
        for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(frame(Bytes(b.begin(),b.begin()+n)),true,true).structureComplete,"truncated request never accepted");
        b.push_back(0);check(!decodeGameFrame(frame(b),true,true).structureComplete,"trailing request bytes rejected");
    }
    check(!decodeGameFrame(frame(body(0x3700,1)),true,true).structureComplete,"unsupported local coordinates remain incomplete");
    check(!decodeGameFrame(frame(body(0x3700,128)),true,true).structureComplete,"unknown bitmap bits rejected");
    NearbyObjects model,unknown;auto plain=frame(body(0x3701,2));
    check(!unknown.outbound(plain,2,200) && unknown.objects().empty(),"TX request cannot invent self identity");
    model.message(selfAppearFixture(0x3fffffff),10,1000);
    check(!model.outbound(plain,9,900),"request older than current appearance is ignored");
    check(model.outbound(plain,11,1100) && (*model.objects().at(1).position)[0]==10,"known self moves to decoded XYZ");
    check(model.objects().at(1).positionSource=="client_report","client report is not labeled server confirmation");
    check(model.outbound(frame(body(0x3701,2,11)),11,1100) && (*model.objects().at(1).position)[0]==11,"multiple movement frames in one TCP packet retain the last position");
    check(!model.outbound(frame(body(0x3701,2,std::numeric_limits<float>::infinity())),12,1200),"non-finite request is rejected");
    model.message(frame(*parseHex("1d 37 01 02 01 00 00 00 00")),13,1300);
    check((*model.objects().at(1).position)[0]==2.5,"RX delta uses its network baseline, never the client report baseline");
    check(!model.outbound(plain,12,1200),"old imported TX cannot rewind newer server coordinates");
    Bytes enter{0x21,0x36};enter.insert(enter.end(),46,0);model.message(frame(enter),14,1400);
    check(!model.outbound(plain,15,1500),"scene reset clears self association");
    Stream discovered;auto appearance=selfAppearFixture(0x3fffffff);discovered.bytes=appearance;
    auto heartbeat=frame(*parseHex("00 36 00 00 00 00 00 00 00 00"));discovered.bytes.insert(discovered.bytes.end(),heartbeat.begin(),heartbeat.end());
    NearbyObjects delayed;delayed.feed(discovered,1,100,appearance.size());delayed.feed(discovered,20,2000,discovered.bytes.size());
    check(delayed.objects().at(1).firstPacket==1 && delayed.outbound(plain,10,1000),"delayed accelerator discovery preserves appearance chronology for historical TX import");

    CipherSnapshot state;state.source.port=1234;state.destination.port=13328;state.frameSequence=0xfffffff0;
    for(size_t i=0;i<256;++i)state.table[i]=uint8_t(i);
    auto enc=state;Bytes wire;
    for(float x:{10.f,11.f}){auto f=frame(body(0x3701,2,x));auto split=splitGameFrames(f);enc.transform(std::span(f).subspan(split.frames[0].prefixBytes));wire.insert(wire.end(),f.begin(),f.end());}
    Stream stream;auto p=packet(state,state.frameSequence,Bytes(wire.begin(),wire.begin()+5));stream.add(p);
    std::vector<StreamObservation> observations{{5,2,200}};SelfMovementStream decoder;NearbyObjects tracked;tracked.message(selfAppearFixture(0x3fffffff),1,100);
    decoder.feed(stream,state.source,state.destination,state,observations,tracked);check(decoder.frames==0,"split frame does not advance cipher state");
    p=packet(state,state.frameSequence+15,Bytes(wire.begin()+15,wire.end()));stream.add(p);
    decoder.feed(stream,state.source,state.destination,state,observations,tracked);check(decoder.frames==0 && stream.hasGap(),"TCP gap is never decrypted across");
    p=packet(state,state.frameSequence+5,Bytes(wire.begin()+5,wire.begin()+15));stream.add(p);observations.push_back({wire.size(),4,400});
    decoder.feed(stream,state.source,state.destination,state,observations,tracked);
    check(decoder.frames==2 && decoder.updates==2 && (*tracked.objects().at(1).position)[0]==11,"gap closure and sequence wrap decrypt both frames in wire order");
    stream.add(p);decoder.feed(stream,state.source,state.destination,state,observations,tracked);
    check(decoder.frames==2 && decoder.updates==2,"retransmission cannot replay an applied report");
    SelfMovementStream other;other.feed(stream,state.destination,state.source,state,observations,tracked);check(other.frames==0,"wrong direction or endpoint cannot consume state");
    Stream next;next.add(packet(state,state.frameSequence+1000,wire));SelfMovementStream nextGeneration;
    nextGeneration.feed(next,state.source,state.destination,state,observations,tracked);check(nextGeneration.frames==0,"new generation without the snapshot anchor remains undecoded");

    if(argc>2){
        const auto captured=loadCipherSnapshot(argv[2]);Stream live;std::vector<StreamObservation> seen;SelfMovementStream replay;
        NearbyObjects fixture;fixture.message(selfAppearFixture(0x3fffffff),0,0);size_t matching=0,decoded=0;std::array<size_t,4> counts{};
        for(const auto& record:loadPcap(argv[1]))if(record.protocol==6 && record.source==captured.source && record.destination==captured.destination){
            ++matching;const auto before=live.bytes.size();live.add(record);if(live.bytes.size()>before)seen.push_back({live.bytes.size(),record.id,record.timeUs});
            replay.feed(live,record.source,record.destination,captured,seen,fixture);
        }
        auto bytes=decryptGameStream(live.bytes,live.initialSequence+(live.startedWithSyn?1:0),captured.source,captured.destination,captured);
        std::optional<std::array<double,3>> first,last;
        for(const auto& f:splitGameFrames(bytes).frames){auto b=std::span(bytes).subspan(f.offset,f.length);auto op=readInteger(b,f.prefixBytes,2,false);if(op<0x3700 || op>0x3703)continue;
            auto m=decodeGameFrame(b,true,true);check(m.structureComplete,"real moving-client request completely decoded");++counts[op-0x3700];++decoded;
            std::array<double,3> xyz{};for(auto& field:m.fields)for(size_t i=0;i<3;++i)if(field.name==std::string(1,"XYZ"[i]))xyz[i]=std::bit_cast<float>(uint32_t(readInteger(b,field.offset,4,false)));
            if(!first)first=xyz;last=xyz;
        }
        check(decoded==267 && replay.updates==decoded,"all 267 real coordinate requests reach the fixture self model exactly once");
        check(first && last && *first!=*last && fixture.objects().at(1).position==last,"capture coordinates change and final fixture position equals last TX request");
        std::cout<<"live TX counts="<<counts[0]<<','<<counts[1]<<','<<counts[2]<<','<<counts[3]<<" updates="<<replay.updates<<" first="<<(*first)[0]<<','<<(*first)[1]<<','<<(*first)[2]<<" last="<<(*last)[0]<<','<<(*last)[1]<<','<<(*last)[2]<<" (self identity is a synthetic fixture; capture began mid-session)\n";
    }
    std::cout<<checks<<" self movement checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
