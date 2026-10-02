#include "protocol.hpp"
#include <iostream>
#include <fstream>
#include <random>
#include <stdexcept>
using namespace aion;
static int checks=0;
static void check(bool test,const char* name){++checks;if(!test)throw std::runtime_error(name);}
int main(int argc,char** argv) {try {
    auto demo=demoPackets(); check(demo.size()==5,"demo packets");
    check(demo[1].source.port==50000 && demo[1].destination.port==7777,"network byte order");
    Stream s; for(auto& p:demo) s.add(p);
    check(s.bytes.size()==22 && s.duplicateBytes==7 && !s.hasGap(),"TCP split/reorder/retransmit");
    auto frames=splitFrames(s.bytes,{}); check(frames.frames.size()==2 && frames.consumed==22,"coalesced messages");
    check(frames.frames[0].opcode==1 && frames.frames[1].opcode==2,"opcodes");
    check(splitFrames(std::span(s.bytes).first(13),{}).frames.size()==1,"partial header");
    check(splitFrames(std::span(s.bytes).first(20),{}).consumed==12,"partial body");
    auto invalid=s.bytes; invalid[0]=0; check(splitFrames(invalid,{}).frames.empty(),"invalid length");
    Framing be; be.bigEndian=true; check(splitFrames(Bytes{0,4,0,9},be).frames[0].opcode==9,"big endian");
    be.includesHeader=false; check(splitFrames(Bytes{0,0,0,8},be).frames[0].length==4,"body-only length");
    be.width=8; check(splitFrames(s.bytes,be).status=="Invalid framing rule","invalid rule");
    Framing shortRule; shortRule.width=1; shortRule.headerSize=1; shortRule.opcodeWidth=0; shortRule.adjustment=-3;
    check(splitFrames(Bytes{6,0xaa,0xbb,5,0xcc},shortRule).frames.size()==2,"signed length adjustment");
    check(splitFrames(Bytes{1},shortRule).frames.empty(),"negative length rejection");
    Stream gap; gap.add(demo[0]); gap.add(demo[2]); check(gap.bytes.empty() && gap.hasGap(),"gap cannot be concatenated");
    Packet p; p.protocol=6; p.sequence=0xfffffffc; p.payload={1,2,3,4}; Stream wrap; wrap.add(p); p.sequence=2; p.payload={7,8}; wrap.add(p); p.sequence=0; p.payload={5,6}; wrap.add(p);
    check(wrap.bytes==Bytes({1,2,3,4,5,6,7,8}) && !wrap.hasGap(),"TCP sequence wrap");
    p.sequence=1; p.payload={6,7,8,9}; wrap.add(p); check(wrap.bytes.back()==9 && wrap.bytes.size()==9,"partial overlap");
    check(parseHex("AA bb 01")==Bytes({0xaa,0xbb,1}),"hex search"); check(!parseHex("A B C") && !parseHex("GG"),"invalid hex");
    check(readInteger(Bytes{1,2,3,4},0,4,false)==0x04030201,"LE integer");
    bool threw=false; try {readInteger(Bytes{1},1,8,false);}catch(...){threw=true;} check(threw,"field bounds");
    Packet parsed; std::string error;
    auto raw=demo[1].raw; raw[6]=0x20; check(!parsePacket(raw,parsed,error),"reject fragmented IPv4");
    raw=demo[1].raw; raw[32]=0x10; check(!parsePacket(raw,parsed,error),"TCP header bounds");
    Bytes udp(51); udp[0]=0x60; udp[5]=11; udp[6]=17; udp[7]=64; udp[23]=1; udp[39]=1; udp[40]=0x12;udp[41]=0x34;udp[42]=0x56;udp[43]=0x78;udp[45]=11;udp[48]=1;udp[49]=2;udp[50]=3;
    check(parsePacket(udp,parsed,error) && parsed.ipv6 && parsed.payload==Bytes({1,2,3}),"IPv6 UDP");
    auto path=std::filesystem::temp_directory_path()/"aion2pipe-test.pcap";
    savePcap(path,demo); auto loaded=loadPcap(path); check(loaded.size()==demo.size() && loaded[1].raw==demo[1].raw && loaded[1].timeUs==demo[1].timeUs && !loaded[1].attributed,"PCAP round trip and unknown PID");
    {std::ofstream f(path,std::ios::binary); f<<"garbage";}
    threw=false; try{loadPcap(path);}catch(...){threw=true;} check(threw,"truncated PCAP"); std::filesystem::remove(path);
    std::mt19937 random(20260929);
    for(int i=0;i<10000;++i) {Bytes fuzz(random()%256); for(auto& c:fuzz)c=uint8_t(random()); parsePacket(fuzz,parsed,error); splitFrames(fuzz,{});}
    check(entropy(Bytes(100,0))==0,"zero entropy");
    std::cout<<checks<<" checks and 10000 malformed-input cases passed\n";
    if(argc>1) {
        auto live=loadPcap(argv[1]); std::map<std::string,Stream> streams;
        for(auto& packet:live) if(packet.protocol==6) streams[packet.directionKey()].add(packet);
        size_t count=0;
        for(auto& [key,stream]:streams) {
            auto split=splitFrames(stream.bytes,shortRule); check(!stream.hasGap(),"sample stream gap");
            check(split.consumed==stream.bytes.size(),"sample candidate length mismatch"); count+=split.frames.size();
            std::cout<<"sample stream bytes="<<stream.bytes.size()<<" frames="<<split.frames.size()<<"\n";
        }
        std::cout<<"sample packets="<<live.size()<<" frames="<<count<<"\n";
    }
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
