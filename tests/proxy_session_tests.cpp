#include "protocol.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace aion;
int main(){try{
    auto dir=std::filesystem::temp_directory_path()/"aion-proxy-session-tests";std::filesystem::create_directories(dir);auto path=dir/"roundtrip.a2session";
    Packet p;p.proxyConnection=0x100000002;p.pid=42;p.protocol=6;p.attributed=p.outbound=p.plaintext=true;p.toolGenerated=true;p.sequence=0xfffffff0;p.flags=0x18;p.timeUs=1790700000123456;
    p.source.address[10]=p.source.address[11]=255;p.source.address[12]=127;p.source.address[15]=1;p.source.port=45678;p.destination=p.source;p.destination.port=13328;p.raw={1,2,3,255};p.payload={8,1,2,3,4};
    size_t checks=0;auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("session check failed "+std::to_string(checks));};
    saveSession(path,{p},true);bool incomplete=false;auto v=loadSession(path,&incomplete);check(v.size()==1 && incomplete);auto& q=v[0];
    check(q.proxyConnection==p.proxyConnection && q.pid==42 && q.sequence==p.sequence && q.timeUs==p.timeUs && q.flags==p.flags);
    check(q.source==p.source && q.destination==p.destination && q.raw==p.raw && q.payload==p.payload && q.plaintext && q.outbound && q.toolGenerated);
    auto same=p;same.proxyConnection++;check(same.flowKey()!=p.flowKey());std::swap(same.source,same.destination);same.proxyConnection=p.proxyConnection;check(same.flowKey()==p.flowKey() && same.directionKey()!=p.directionKey());
    auto reject=[&](auto action){bool threw=false;try{action();}catch(...){threw=true;}check(threw);};
    reject([&]{savePcap(dir/"not-ip.pcap",{p});});
    std::ifstream f(path,std::ios::binary);Bytes original((std::istreambuf_iterator<char>(f)),{});f.close();
    for(size_t n=0;n<original.size();++n){saveBinary(path,std::span(original).first(n));reject([&]{loadSession(path);});}
    for(auto at:{0u,8u,12u,41u,78u,82u}){auto corrupt=original;for(size_t i=at;i<std::min(corrupt.size(),size_t(at)+4);++i)corrupt[i]=255;saveBinary(path,corrupt);reject([&]{loadSession(path);});}
    auto trailing=original;trailing.push_back(0);saveBinary(path,trailing);reject([&]{loadSession(path);});
    saveSession(path,{},false);check(loadSession(path,&incomplete).empty() && !incomplete);
    p.proxyConnection=0;reject([&]{saveSession(path,{p});});
    std::filesystem::remove(path);std::filesystem::remove(dir);std::cout<<checks<<" session checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
