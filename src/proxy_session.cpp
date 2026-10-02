#include "protocol.hpp"
#include <fstream>
#include <stdexcept>
#include <algorithm>
namespace aion {
namespace {
constexpr uint64_t MaxBytes=128ull*1024*1024;
void put(std::ostream& f,uint64_t v,size_t n){for(size_t i=0;i<n;++i)f.put(char(v>>(i*8)));}
void bytes(std::ostream& f,std::span<const uint8_t> b){f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));}
void validate(const Packet& p){
    if(!p.proxyConnection || p.protocol!=6 || !p.attributed || p.ipv6 || p.raw.size()>2097152 || p.payload.size()>2097152 || (!p.wireObservation && (p.flags&~0x1f)))throw std::runtime_error("Invalid proxy session record");
    if(p.wireObservation){Packet parsed;std::string error;if(p.plaintext || p.toolGenerated || !parsePacket(p.raw,parsed,error) || parsed.protocol!=6 || parsed.ipv6 || parsed.payload!=p.payload || parsed.source!=p.source || parsed.destination!=p.destination || parsed.sequence!=p.sequence || parsed.flags!=p.flags)throw std::runtime_error("Invalid wire observation");}
}
}
void saveSession(const std::filesystem::path& path,const std::vector<Packet>& packets,bool incomplete){
    if(packets.size()>50000)throw std::runtime_error("Session record limit");
    uint64_t total=0;for(const auto& p:packets){validate(p);total+=p.raw.size()+p.payload.size();if(total>MaxBytes)throw std::runtime_error("Session memory limit");}
    std::ofstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot open session output");
    f.write("A2SESS03",8);put(f,packets.size(),4);put(f,incomplete?1:0,4);
    for(const auto& p:packets){
        put(f,p.proxyConnection,8);put(f,p.timeUs,8);put(f,p.pid,4);put(f,p.sequence,4);
        put(f,p.flags,1);put(f,(p.outbound?1:0)|(p.plaintext?2:0)|(p.toolGenerated?4:0)|(p.wireObservation?8:0),1);
        bytes(f,p.source.address);put(f,p.source.port,2);bytes(f,p.destination.address);put(f,p.destination.port,2);
        put(f,p.raw.size(),4);put(f,p.payload.size(),4);bytes(f,p.raw);bytes(f,p.payload);
    }
    f.close();if(!f)throw std::runtime_error("Session write failed");
}
std::vector<Packet> loadSession(const std::filesystem::path& path,bool* incomplete){
    if(std::filesystem::file_size(path)>MaxBytes+50000ull*70+16)throw std::runtime_error("Session file limit");
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot open session");
    auto read=[&](size_t n){Bytes b(n);if(!f.read(reinterpret_cast<char*>(b.data()),std::streamsize(n)))throw std::runtime_error("Truncated session");return b;};
    auto number=[&](size_t n){auto b=read(n);return readInteger(b,0,n,false);};
    auto magic=read(8);auto version=std::string(magic.begin(),magic.end());if(version!="A2SESS03" && version!="A2SESS02")throw std::runtime_error("Unsupported session version");
    auto count=number(4);if(count>50000)throw std::runtime_error("Session record limit");
    auto status=number(4);if(status>1)throw std::runtime_error("Invalid session completeness flags");if(incomplete)*incomplete=status!=0;
    std::vector<Packet> out;uint64_t total=0;
    for(uint64_t i=0;i<count;++i){
        Packet p;p.id=i+1;p.proxyConnection=number(8);p.timeUs=number(8);p.pid=uint32_t(number(4));p.sequence=uint32_t(number(4));p.flags=uint8_t(number(1));
        auto bits=number(1);if(bits>(version=="A2SESS03"?15u:7u))throw std::runtime_error("Invalid session flags");p.outbound=(bits&1)!=0;p.plaintext=(bits&2)!=0;p.toolGenerated=(bits&4)!=0;p.wireObservation=(bits&8)!=0;p.attributed=true;p.protocol=6;
        auto a=read(16);std::copy(a.begin(),a.end(),p.source.address.begin());p.source.port=uint16_t(number(2));a=read(16);std::copy(a.begin(),a.end(),p.destination.address.begin());p.destination.port=uint16_t(number(2));
        auto raw=number(4),plain=number(4);total+=raw+plain;if(raw>2097152 || plain>2097152 || total>MaxBytes)throw std::runtime_error("Session memory limit");
        p.raw=read(size_t(raw));p.payload=read(size_t(plain));validate(p);out.push_back(std::move(p));
    }
    if(f.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Trailing session bytes");return out;
}
}
