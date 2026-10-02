#include "query_proxy.hpp"
#include "diagnostics.hpp"
#include "nearby.hpp"
#include "self_appear_fixture.hpp"
#include <iostream>
#include <psapi.h>
using namespace aion;
// Local integration harness only. The Python peer supplies both fake game and
// fake server, independently implementing RSA with OpenSSL and standard ARC4.
int main(int argc,char** argv){
    if(argc<2 || argc>4)return 2;const bool flatMode=argc==4 && std::string(argv[3])=="--flat";bool overflow=argc>=3 && std::string(argv[2])=="--overflow",cancelMode=argc>=3 && (std::string(argv[2])=="--jump-cancel" || std::string(argv[2])=="--jump-repeat-cancel"),repeatMode=argc>=3 && (std::string(argv[2])=="--jump-repeat" || std::string(argv[2])=="--jump-repeat-cancel"),jumpMode=argc>=3 && (std::string(argv[2])=="--jump" || cancelMode || repeatMode);int port=atoi(argv[1]);if(port<1024 || port>65535)return 2;
    WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return 2;
    initializeDiagnostics();
    int result=1;
    {
        QueryProxy proxy;if(!proxy.start(L"python.exe",uint16_t(port))){std::cerr<<proxy.status()<<'\n';return 3;}
        if(argc>=3 && std::string(argv[2])=="--lifecycle"){
            const auto count=argc==4?atoi(argv[3]):40;if(count<1 || count>10000)return 2;
            DWORD initialHandles{};GetProcessHandleCount(GetCurrentProcess(),&initialHandles);
            std::cout<<"READY"<<std::endl;size_t highest=0;bool reported=false;auto deadline=GetTickCount64()+180000+uint64_t(count)*1000,nextSample=GetTickCount64();
            while(GetTickCount64()<deadline){
                proxy.takePackets();auto views=proxy.connections();for(const auto& c:views)highest=std::max(highest,c.id);
                if(GetTickCount64()>=nextSample){
                    DWORD handles{};PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
                    GetProcessHandleCount(GetCurrentProcess(),&handles);K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
                    std::cout<<"RESOURCE highest="<<highest<<" retained="<<views.size()<<" handles="<<handles<<" private_bytes="<<memory.PrivateUsage<<std::endl;nextSample=GetTickCount64()+10000;
                }
                if(highest>=size_t(count) && !proxy.busy() && !reported){reported=true;std::cout<<count<<" connections forwarded; checking delayed route cleanup"<<std::endl;}
                if(reported && views.empty()){if(!proxy.stop())return 21;DWORD handles{};GetProcessHandleCount(GetCurrentProcess(),&handles);
                    if(handles>initialHandles+8){std::cerr<<"Handle growth after cleanup: "<<initialHandles<<" -> "<<handles<<std::endl;return 22;}
                    std::cout<<"PASS: reconnect admission, retired routes and thread cleanup; handles "<<initialHandles<<" -> "<<handles<<std::endl;return 0;}
                Sleep(50);
            }
            std::cerr<<"Lifecycle timeout, highest="<<highest<<" remaining="<<proxy.connections().size()<<std::endl;return 20;
        }
        saveBinary("artifacts/field-analysis/proxy-self-fixture.bin",selfAppearFixture(0x3fffffff));
        std::vector<Packet> observed;
        std::cout<<"READY"<<std::endl;if(overflow)Sleep(2000);bool submitted=false,responded=false,jumpSubmitted=false,repeatStopped=false;std::map<size_t,std::string> last;auto end=GetTickCount64()+30000;
        while(GetTickCount64()<end){
            auto batch=proxy.takePackets();for(auto& p:batch)observed.push_back(std::move(p));
            for(auto c:proxy.connections()){
                if(last[c.id]!=c.status){last[c.id]=c.status;std::cerr<<last[c.id]<<std::endl;}
                if(!submitted && c.ready){if(proxy.stop()){std::cerr<<"stop unexpectedly accepted active connection\n";return 4;}submitted=proxy.request(c.id,2017,123456);}
                if(!c.response.fields.empty())responded=true;
                if(jumpMode && responded && !jumpSubmitted && c.jumpState.ready){if(flatMode && (!c.jumpState.simulated || c.jumpState.learned)){std::cerr<<"flat mode unexpectedly required native jump learning\n";return 12;}if(repeatMode && (proxy.setJumpRepeat(c.id,true,4) || proxy.setJumpRepeat(c.id,true,3601))){std::cerr<<"invalid repeat interval accepted\n";return 10;}
                    jumpSubmitted=repeatMode?proxy.setJumpRepeat(c.id,true,6):proxy.requestJump(c.id);if(jumpSubmitted && proxy.requestJump(c.id)){std::cerr<<"duplicate jump accepted\n";return 9;}}
                if(repeatMode && cancelMode && c.jumpsSent>=1 && !c.jumpRepeat && !c.jumpPending)repeatStopped=true;
                if(repeatMode && !cancelMode && c.jumpsSent>=2 && !c.jumpPending && c.jumpRepeat){repeatStopped=proxy.setJumpRepeat(c.id,false);}
                if(!c.open && c.status.starts_with("连接结束："))std::cerr<<c.status<<std::endl;
            }
            if(responded && (!jumpMode || jumpSubmitted) && !proxy.busy()){result=proxy.stop()?0:5;break;}
            Sleep(30);
        }
        auto batch=proxy.takePackets();for(auto& p:batch)observed.push_back(std::move(p));
        auto path=std::filesystem::path(overflow?"artifacts/field-analysis/unified-overflow.a2session":jumpMode?"artifacts/field-analysis/jump-proxy.a2session":"artifacts/field-analysis/unified-proxy.a2session");saveSession(path,observed,proxy.stats().queueDropped!=0);
        bool incomplete=false;auto replay=loadSession(path,&incomplete);std::vector<Packet> wire;for(const auto& p:replay)if(p.wireObservation)wire.push_back(p);
        auto pcap=path;pcap.replace_extension(".pcap");savePcap(pcap,wire);auto wireReplay=loadPcap(pcap);if(wire.empty() || wireReplay.size()!=wire.size())result=8;else for(size_t i=0;i<wire.size();++i)if(wireReplay[i].raw!=wire[i].raw)result=8;
        NearbyObjects model;size_t tx=0,tool=0;uint64_t index=0;
        for(const auto& p:replay){++index;if(p.payload.empty() || p.wireObservation)continue;
            if(p.outbound){if(p.plaintext && !p.toolGenerated){++tx;model.outbound(p.payload,index,p.timeUs);}if(p.toolGenerated)++tool;}
            else model.message(p.payload,index,p.timeUs);
        }
        if(overflow){if(!incomplete || !proxy.stats().queueDropped || observed.size()!=8192)result=7;}
        else if(replay.size()!=observed.size() || tx!=(jumpMode?((repeatMode && !cancelMode?17u:16u)-(flatMode?9u:0u)):6u) || tool!=(jumpMode?(cancelMode?2u:(repeatMode?(flatMode?23u:21u):(flatMode?12u:11u))):1u) || !model.objects().contains(1) || !model.objects().at(1).position || (*model.objects().at(1).position)[0]!=99 || proxy.stats().queueDropped){std::cerr<<"unified observations/replay mismatch tx="<<tx<<" tool="<<tool<<std::endl;result=6;}
        if(repeatMode && !repeatStopped)result=11;
        std::cout<<"observations="<<observed.size()<<" self_x="<<(model.objects().contains(1)&&model.objects().at(1).position?(*model.objects().at(1).position)[0]:-1)<<" replay=verified dropped="<<proxy.stats().queueDropped<<"\n";
        std::cout<<"submitted="<<submitted<<" responded="<<responded<<" result="<<result<<std::endl;
    }
    WSACleanup();return result;
}
