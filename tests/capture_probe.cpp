#include "capture.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>
using namespace aion;
int wmain(int argc,wchar_t** argv) {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2),&wsa);
    std::wstring target=argc>1?argv[1]:L"capture_probe.exe";
    int seconds=argc>2?_wtoi(argv[2]):3;
    Capture capture;
    if(!capture.start(target)) {std::cerr<<capture.status()<<'\n'; return 2;}
    SOCKET server=INVALID_SOCKET,client=INVALID_SOCKET;
    if(argc==1) {
        server=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); client=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in endpoint{}; endpoint.sin_family=AF_INET; endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(bind(server,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint))==SOCKET_ERROR)return 3;
        int len=sizeof(endpoint); getsockname(server,reinterpret_cast<sockaddr*>(&endpoint),&len);
        for(int i=0;i<10;++i){sendto(client,"Aion2Pipe probe",15,0,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint)); std::this_thread::sleep_for(std::chrono::milliseconds(40));}
    }
    std::vector<Packet> packets;
    for(int i=0;i<seconds*10;++i) {std::this_thread::sleep_for(std::chrono::milliseconds(100)); auto batch=capture.drain(); for(auto& p:batch)packets.push_back(std::move(p));}
    capture.stop(); auto batch=capture.drain(); for(auto& p:batch)packets.push_back(std::move(p));
    auto stats=capture.stats(); std::cout<<"received="<<stats.received<<" matched="<<stats.matched<<" dropped="<<stats.queueDropped<<" unsupported="<<stats.unsupported<<" targetProcesses="<<stats.processes<<"\n";
    if(argc>3)savePcap(argv[3],packets);
    size_t payloads=0;
    for(auto& p:packets) if(!p.payload.empty()) {++payloads; std::cout<<p.pid<<' '<<(p.outbound?"TX ":"RX ")<<p.source.text()<<" -> "<<p.destination.text()<<" size="<<p.payload.size()<<"\n";}
    if(server!=INVALID_SOCKET)closesocket(server); if(client!=INVALID_SOCKET)closesocket(client); WSACleanup();
    std::cout<<"payloads="<<payloads<<"\n"; return argc==1 && payloads<10 ? 1:0;
}
