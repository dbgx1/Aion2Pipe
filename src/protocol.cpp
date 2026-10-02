#include "protocol.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace aion {
uint64_t readInteger(std::span<const uint8_t> b, size_t o, size_t w, bool be) {
    if (!w || w > 8 || o > b.size() || w > b.size() - o) throw std::out_of_range("Field exceeds data bounds");
    uint64_t v = 0;
    for (size_t i = 0; i < w; ++i) v |= uint64_t(b[o+i]) << (8 * (be ? w-1-i : i));
    return v;
}
static bool isV4(const Endpoint& e) {
    return std::all_of(e.address.begin(), e.address.begin()+10, [](auto b){ return b == 0; }) && e.address[10] == 255 && e.address[11] == 255;
}
bool Endpoint::wildcard() const {
    const size_t start = isV4(*this) ? 12 : 0;
    return std::all_of(address.begin()+start, address.end(), [](auto b){return b == 0;});
}
std::string Endpoint::text() const {
    char out[INET6_ADDRSTRLEN]{};
    const bool v4 = isV4(*this);
    InetNtopA(v4 ? AF_INET : AF_INET6, const_cast<uint8_t*>(address.data() + (v4 ? 12 : 0)), out, sizeof(out));
    return (v4 ? std::string(out) : "[" + std::string(out) + "]") + ":" + std::to_string(port);
}
std::string Packet::flowKey() const {
    auto a=source.text(), b=destination.text();
    if (a>b) std::swap(a,b);
    return (wireObservation?"wire/":"")+ (proxyConnection?"proxy:"+std::to_string(proxyConnection)+"/":"")+std::to_string(pid)+"/"+std::to_string(protocol)+"/"+a+"/"+b;
}
std::string Packet::directionKey() const { return flowKey()+"/"+source.text(); }
bool parsePacket(std::span<const uint8_t> b, Packet& p, std::string& error) {
    auto fail=[&](const char* s){ error=s; return false; };
    error.clear();
    if (b.empty()) return fail("Empty IP packet");
    p.source={}; p.destination={}; p.flags=0; p.sequence=0; p.payload.clear(); p.raw.clear();
    size_t offset=0, total=0;
    if ((b[0] >> 4) == 4) {
        if (b.size()<20) return fail("Truncated IPv4 header");
        offset=(b[0]&15)*4; total=readInteger(b,2,2,true);
        if(offset<20 || total<offset || total>b.size()) return fail("Invalid IPv4 length");
        if(readInteger(b,6,2,true)&0x3fff) return fail("IP fragments require reassembly; packet skipped");
        p.ipv6=false; p.protocol=b[9];
        p.source.address[10]=p.source.address[11]=p.destination.address[10]=p.destination.address[11]=255;
        std::copy_n(b.begin()+12,4,p.source.address.begin()+12);
        std::copy_n(b.begin()+16,4,p.destination.address.begin()+12);
    } else if ((b[0] >> 4) == 6) {
        if(b.size()<40) return fail("Truncated IPv6 header");
        total=40+readInteger(b,4,2,true); offset=40; p.protocol=b[6]; p.ipv6=true;
        if(total>b.size()) return fail("Invalid IPv6 length");
        std::copy_n(b.begin()+8,16,p.source.address.begin());
        std::copy_n(b.begin()+24,16,p.destination.address.begin());
        for(int i=0; p.protocol!=6 && p.protocol!=17; ++i) {
            if(i>=16 || offset+2>total) return fail("Invalid IPv6 extension chain");
            const auto type=p.protocol; p.protocol=b[offset]; size_t n=0;
            if(type==0 || type==43 || type==60) n=(size_t(b[offset+1])+1)*8;
            else if(type==51) n=(size_t(b[offset+1])+2)*4;
            else if(type==44) {
                n=8;
                if(offset+n>total || (readInteger(b,offset+2,2,true)&0xfff9)) return fail("IPv6 fragment skipped");
            } else return fail("Unsupported IP protocol");
            if(offset+n>total) return fail("Truncated IPv6 extension");
            offset+=n;
        }
    } else return fail("Unknown IP version");
    if(p.protocol!=6 && p.protocol!=17) return fail("Not TCP or UDP");
    const size_t minHeader=p.protocol==6 ? 20 : 8;
    if(offset+minHeader>total) return fail("Truncated transport header");
    p.source.port=uint16_t(readInteger(b,offset,2,true));
    p.destination.port=uint16_t(readInteger(b,offset+2,2,true));
    size_t header=minHeader;
    if(p.protocol==6) {
        header=(b[offset+12]>>4)*4;
        if(header<20 || offset+header>total) return fail("Invalid TCP header length");
        p.sequence=uint32_t(readInteger(b,offset+4,4,true)); p.flags=b[offset+13];
    } else {
        const auto len=readInteger(b,offset+4,2,true);
        if(len<8 || offset+len>total) return fail("Invalid UDP length");
        total=offset+size_t(len);
    }
    p.raw.assign(b.begin(), b.end());
    p.payload.assign(b.begin()+offset+header,b.begin()+total);
    return true;
}
std::optional<Bytes> parseHex(const std::string& s) {
    Bytes out; int hi=-1;
    for(unsigned char c:s) {
        if(c==' ' || c=='\t' || c=='\r' || c=='\n') continue;
        int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
        if(n<0) return std::nullopt;
        if(hi<0) hi=n; else {out.push_back(uint8_t((hi<<4)|n)); hi=-1;}
    }
    if(hi>=0) return std::nullopt;
    return out;
}
std::string hex(std::span<const uint8_t> b) {
    static const char* digits="0123456789ABCDEF";
    std::string s; s.reserve(b.size()*3);
    for(auto v:b) { if(!s.empty()) s+=' '; s+=digits[v>>4]; s+=digits[v&15]; }
    return s;
}
double entropy(std::span<const uint8_t> b) {
    if(b.empty()) return 0;
    std::array<size_t,256> counts{}; for(auto v:b) ++counts[v];
    double h=0; for(auto c:counts) if(c) {double f=double(c)/double(b.size()); h-=f*std::log2(f);} return h;
}
void Stream::add(const Packet& p) {
    if(p.protocol!=6 || limited) return;
    uint32_t seq=p.sequence+((p.flags&2)?1:0);
    if(!initialized) {initialized=true; next=seq; initialSequence=p.sequence; startedWithSyn=(p.flags&2)!=0;}
    if(p.flags&5) closed=true;
    if(p.payload.empty()) return;
    auto insert=[&](uint32_t at, std::span<const uint8_t> data) {
        int32_t delta=static_cast<int32_t>(at-next);
        if(delta<0) {
            const auto skip=std::min<size_t>(size_t(-int64_t(delta)),data.size());
            duplicateBytes+=skip; data=data.subspan(skip); at+=uint32_t(skip);
        }
        if(data.empty()) return;
        if(at==next) {
            if(bytes.size()+data.size()>Limit) {limited=true; return;}
            bytes.insert(bytes.end(),data.begin(),data.end()); next+=uint32_t(data.size());
        } else {
            if(pendingBytes+data.size()>Limit || pending.size()>=4096) {limited=true; return;}
            auto it=pending.find(at);
            if(it==pending.end()) {pending[at]=Bytes(data.begin(),data.end()); pendingBytes+=data.size();}
            else if(data.size()>it->second.size()) {pendingBytes+=data.size()-it->second.size(); it->second.assign(data.begin(),data.end());}
        }
    };
    insert(seq,p.payload);
    // Scan using serial-number arithmetic: numeric map ordering fails at 2^32 wraparound.
    bool progress=true;
    while(progress && !limited) {
        progress=false;
        for(auto it=pending.begin(); it!=pending.end(); ++it) if(static_cast<int32_t>(it->first-next)<=0) {
            auto at=it->first; auto data=std::move(it->second); pendingBytes-=data.size(); pending.erase(it);
            insert(at,data); progress=true; break;
        }
    }
}
FrameResult splitFrames(std::span<const uint8_t> b, const Framing& r) {
    FrameResult out;
    if((r.width!=1 && r.width!=2 && r.width!=4) || r.headerSize==0 || r.headerSize>r.maxFrame || r.offset>r.headerSize || r.width>r.headerSize-r.offset || r.opcodeWidth>8) {
        out.status="Invalid framing rule"; return out;
    }
    while(out.consumed<b.size()) {
        auto remaining=b.subspan(out.consumed);
        if(remaining.size()<r.headerSize) {out.status="Waiting for complete header"; break;}
        int64_t signedLength=int64_t(readInteger(remaining,r.offset,r.width,r.bigEndian))+r.adjustment;
        if(!r.includesHeader) signedLength+=int64_t(r.headerSize);
        if(signedLength<int64_t(r.headerSize) || uint64_t(signedLength)>r.maxFrame) {out.status="Invalid length at offset "+std::to_string(out.consumed)+"; adjust rule or start offset"; break;}
        auto length=uint64_t(signedLength);
        if(length>remaining.size()) {out.status="Waiting for "+std::to_string(length-remaining.size())+" more bytes"; break;}
        Frame f{out.consumed,size_t(length),0,false};
        if(r.opcodeWidth && r.opcodeOffset<=length && r.opcodeWidth<=length-r.opcodeOffset) {
            f.opcode=readInteger(remaining,r.opcodeOffset,r.opcodeWidth,r.bigEndian); f.hasOpcode=true;
        }
        out.frames.push_back(f); out.consumed+=size_t(length);
        if(out.frames.size()>=100000) {out.status="Frame display limit reached"; break;}
    }
    if(out.status.empty()) out.status="Complete";
    return out;
}
static void writeLE(std::ostream& f, uint64_t v, int n) {for(int i=0;i<n;++i) f.put(char(v>>(i*8)));}
void saveBinary(const std::filesystem::path& path, std::span<const uint8_t> b) {
    std::ofstream f(path,std::ios::binary); if(!f) throw std::runtime_error("Cannot open output file");
    f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size())); f.close();
    if(!f) throw std::runtime_error("Write failed");
}
void savePcap(const std::filesystem::path& path,const std::vector<Packet>& packets) {
    if(std::any_of(packets.begin(),packets.end(),[](const Packet& p){return p.proxyConnection!=0 && !p.wireObservation;}))throw std::runtime_error("代理消息不是 IP 包，请保存 .a2session 会话");
    std::ofstream f(path,std::ios::binary); if(!f) throw std::runtime_error("Cannot open PCAP output");
    writeLE(f,0xa1b2c3d4,4); writeLE(f,2,2); writeLE(f,4,2); writeLE(f,0,4); writeLE(f,0,4); writeLE(f,65535,4); writeLE(f,101,4);
    for(const auto& p:packets) {
        if(p.raw.empty()) continue;
        writeLE(f,p.timeUs/1000000,4); writeLE(f,p.timeUs%1000000,4);
        writeLE(f,p.raw.size(),4); writeLE(f,p.raw.size(),4);
        f.write(reinterpret_cast<const char*>(p.raw.data()),std::streamsize(p.raw.size()));
    }
    f.close(); if(!f) throw std::runtime_error("PCAP write failed");
}
std::vector<Packet> loadPcap(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary); if(!f) throw std::runtime_error("Cannot open PCAP");
    auto read=[&](size_t n){Bytes b(n); if(!f.read(reinterpret_cast<char*>(b.data()),std::streamsize(n))) throw std::runtime_error("Truncated PCAP"); return b;};
    auto hdr=read(24); auto magic=readInteger(hdr,0,4,false);
    bool be=magic==0xd4c3b2a1 || magic==0x4d3cb2a1;
    bool nano=magic==0xa1b23c4d || magic==0x4d3cb2a1;
    if(magic!=0xa1b2c3d4 && magic!=0xd4c3b2a1 && !nano) throw std::runtime_error("Use classic PCAP (PCAPNG is not supported)");
    if(readInteger(hdr,4,2,be)!=2 || readInteger(hdr,6,2,be)!=4) throw std::runtime_error("Unsupported PCAP version");
    auto link=readInteger(hdr,20,4,be);
    if(link!=101 && link!=1 && link!=228 && link!=229) throw std::runtime_error("PCAP requires raw IP or Ethernet link type");
    std::vector<Packet> out; size_t memory=0;
    while(f.peek()!=EOF) {
        auto rh=read(16); const auto len=readInteger(rh,8,4,be), orig=readInteger(rh,12,4,be);
        if(len>65535+64 || len>orig) throw std::runtime_error("Invalid PCAP record size");
        auto b=read(size_t(len)); size_t off=0;
        if(link==1) {
            if(b.size()<14) continue;
            off=14; auto type=readInteger(b,12,2,true);
            while(type==0x8100 || type==0x88a8) {if(off+4>b.size()) break; type=readInteger(b,off+2,2,true); off+=4;}
            if(type!=0x0800 && type!=0x86dd) continue;
        }
        Packet p; std::string err;
        if(!parsePacket(std::span(b).subspan(off),p,err)) continue;
        p.timeUs=readInteger(rh,0,4,be)*1000000+readInteger(rh,4,4,be)/(nano?1000:1);
        p.id=out.size()+1;
        memory+=p.raw.size()+p.payload.size();
        if(memory>128*1024*1024 || out.size()>=50000) throw std::runtime_error("PCAP exceeds 128 MiB / 50000 packets; split the file first");
        out.push_back(std::move(p));
    }
    return out;
}
std::vector<Packet> demoPackets() {
    // Synthetic length-prefixed messages, intentionally split, reordered and retransmitted.
    Bytes data={12,0,1,0,0x41,0x49,0x4f,0x4e,0x32,0,1,0, 10,0,2,0,0x64,0,0,0,0x10,0x27};
    std::vector<Packet> out;
    auto add=[&](uint32_t seq,Bytes payload,uint8_t flags) {
        Bytes raw(40+payload.size()); raw[0]=0x45; raw[2]=uint8_t(raw.size()>>8); raw[3]=uint8_t(raw.size()); raw[8]=64; raw[9]=6;
        raw[12]=127; raw[15]=1; raw[16]=127; raw[19]=1;
        raw[20]=0xc3; raw[21]=0x50; raw[22]=0x1e; raw[23]=0x61;
        for(int i=0;i<4;++i) raw[24+i]=uint8_t(seq>>(24-i*8));
        raw[32]=0x50; raw[33]=flags; std::copy(payload.begin(),payload.end(),raw.begin()+40);
        Packet p; std::string e; parsePacket(raw,p,e); p.pid=4242; p.attributed=true; p.outbound=true;
        p.id=out.size()+1; p.timeUs=1700000000000000ull+p.id*12000; out.push_back(std::move(p));
    };
    add(100,{},2); add(101,Bytes(data.begin(),data.begin()+7),0x18);
    add(113,Bytes(data.begin()+12,data.end()),0x18);
    add(108,Bytes(data.begin()+7,data.begin()+12),0x18);
    add(101,Bytes(data.begin(),data.begin()+7),0x18);
    return out;
}
}
