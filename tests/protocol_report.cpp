#include "game_protocol.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace aion;
static std::string quote(const std::string& value){std::string s="\"";for(unsigned char c:value){if(c=='"'||c=='\\'){s+='\\';s+=char(c);}else if(c<32){const char* h="0123456789abcdef";s+="\\u00";s+=h[c>>4];s+=h[c&15];}else s+=char(c);}return s+'"';}
struct Group {Stream stream;std::vector<const Packet*> packets;bool inbound{},proxy{};GameProfile profile=GameProfile::World;};
static void writeConfig(std::ostream& out,const ConfigDocument& config){
    out<<",\"config\":{\"complete\":"<<(config.complete?"true":"false")<<",\"status\":"<<quote(config.status)<<",\"expanded_bytes\":"<<config.expandedBytes<<",\"top_level_keys\":"<<config.topLevelKeys<<",\"values\":[";
    bool first=true;for(const auto& v:config.values){if(!first)out<<',';first=false;out<<"{\"path\":"<<quote(v.path)<<",\"type\":"<<quote(v.type)<<",\"value\":"<<quote(v.value)<<'}';}out<<"]}";
}
static void writeFields(std::ostream& out,const GameMessage& m){
    out<<'[';bool first=true;
    for(const auto& f:m.fields){if(!first)out<<',';first=false;out<<"{\"offset\":"<<f.offset<<",\"size\":"<<f.size<<",\"bit\":"<<f.bit<<",\"name\":"<<quote(f.name)<<",\"type\":"<<quote(f.type)<<",\"value\":"<<quote(f.value)<<",\"meaning_known\":"<<(f.meaningKnown?"true":"false")<<",\"evidence\":"<<quote(f.evidence)<<'}';}out<<']';
}
static void writeExpanded(std::ostream& out,const GameMessage& parent,size_t depth,size_t& budget){
    out<<'[';bool first=true;
    if(!parent.expanded.empty() && depth<4 && parent.expanded.size()<=budget){
        budget-=parent.expanded.size();auto split=splitGameFrames(parent.expanded);
        for(auto& f:split.frames){auto m=decodeGameFrame(std::span(parent.expanded).subspan(f.offset,f.length),true);if(!first)out<<',';first=false;
            out<<"{\"decompressed_offset\":"<<f.offset<<",\"length\":"<<f.length<<",\"name\":"<<quote(m.name)<<",\"structure_complete\":"<<(m.structureComplete?"true":"false")<<",\"status\":"<<quote(m.status)<<",\"fields\":";
            writeFields(out,m);writeConfig(out,m.config);out<<",\"children\":";writeExpanded(out,m,depth+1,budget);out<<'}';
        }
    }else if(!parent.expanded.empty())out<<"{\"status\":\"expanded-report depth or byte limit reached\"}";
    out<<']';
}
int main(int argc,char** argv){try{
    if(argc<3){std::cerr<<"protocol_report sample.pcap|sample.a2session report.jsonl [server-port=13328] [state.a2cs]\n";return 2;}
    std::optional<CipherSnapshot> cipher;if(argc>4)cipher=loadCipherSnapshot(argv[4]);
    bool incomplete=false;auto packets=std::filesystem::path(argv[1]).extension()==".a2session"?loadSession(argv[1],&incomplete):loadPcap(argv[1]);int port=argc>3?std::stoi(argv[3]):13328;
    const auto profile=port==13700?GameProfile::Login:GameProfile::World;
    std::map<std::string,Group> groups;std::map<std::string,size_t> generations;size_t selectedPackets=0;
    for(auto& p:packets){if(p.wireObservation || p.protocol!=6 || (!p.proxyConnection && p.source.port!=port && p.destination.port!=port))continue;
        ++selectedPackets;
        auto flow=p.flowKey(),base=p.directionKey();auto gen=generations[flow];auto key=base+"/"+std::to_string(gen);
        auto it=groups.find(key);
        if((p.flags&2) && !(p.flags&16) && it!=groups.end() && it->second.stream.initialized && (it->second.stream.initialSequence!=p.sequence || it->second.stream.closed))key=base+"/"+std::to_string(++generations[flow]);
        auto& g=groups[key];g.stream.add(p);g.packets.push_back(&p);g.proxy=p.proxyConnection!=0;g.inbound=g.proxy?!p.outbound:p.source.port==port;g.profile=g.proxy?(p.source.port==13700 || p.destination.port==13700?GameProfile::Login:GameProfile::World):profile;
    }
    std::ofstream out(argv[2],std::ios::binary);if(!out)throw std::runtime_error("Cannot create report");size_t count=0,complete=0,inboundCount=0;
    out<<"{\"analysis_incomplete\":"<<(incomplete?"true":"false")<<",\"kind\":\"capture\",\"packet_count\":"<<packets.size()<<",\"profile_server_port\":"<<port<<",\"selected_tcp_packets\":"<<selectedPackets<<",\"outside_profile_packets\":"<<(packets.size()-selectedPackets)<<"}\n";
    for(auto& [key,g]:groups){auto& s=g.stream;Bytes decrypted;auto base=s.initialSequence+(s.startedWithSyn?1:0);std::string decryptError;
        if(cipher && !g.proxy && !g.inbound && !g.packets.empty())try{decrypted=decryptGameStream(s.bytes,base,g.packets[0]->source,g.packets[0]->destination,*cipher);base=cipher->frameSequence;}catch(const std::exception& e){decryptError=e.what();}
        auto data=decrypted.empty()?std::span(s.bytes):std::span(decrypted);auto frames=splitGameFrames(data);
        const auto skipped=uint32_t(base-(s.initialSequence+(s.startedWithSyn?1:0)));
        out<<"{\"kind\":\"stream\",\"stream\":"<<quote(key)<<",\"inbound_by_port\":"<<(g.inbound?"true":"false")<<",\"bytes\":"<<s.bytes.size()<<",\"analysis_bytes\":"<<data.size()<<",\"offset_base_tcp_sequence\":"<<base<<",\"skipped_before_anchor\":"<<skipped<<",\"consumed\":"<<frames.consumed<<",\"gap\":"<<(s.hasGap()?"true":"false")<<",\"midstream\":"<<(!s.startedWithSyn?"true":"false")<<",\"status\":"<<quote(frames.status)<<"}\n";
        if(!decryptError.empty())out<<"{\"kind\":\"decryption_error\",\"stream\":"<<quote(key)<<",\"error\":"<<quote(decryptError)<<"}\n";
        for(auto& f:frames.frames){auto m=decodeGameFrame(data.subspan(f.offset,f.length),g.inbound||(g.proxy && g.profile==GameProfile::World)||!decrypted.empty(),(g.proxy && !g.inbound && g.profile==GameProfile::World)||!decrypted.empty(),g.profile);++count;if(g.inbound)++inboundCount;if(m.structureComplete)++complete;
            out<<"{\"kind\":\"message\",\"stream\":"<<quote(key)<<",\"inbound_by_port\":"<<(g.inbound?"true":"false")<<",\"offset\":"<<f.offset<<",\"length\":"<<f.length<<",\"name\":"<<quote(m.name)<<",\"structure_complete\":"<<(m.structureComplete?"true":"false")<<",\"status\":"<<quote(m.status)<<",\"packet_ids\":[";
            bool first=true,generated=false,native=false;uint64_t time=0,connection=0;
            for(auto p:g.packets){int64_t start=int32_t(p->sequence+((p->flags&2)?1:0)-base);auto end=start+int64_t(p->payload.size());if(!p->payload.empty() && end>int64_t(f.offset) && start<int64_t(f.offset+f.length)){if(!first)out<<',';first=false;out<<p->id;if(!time)time=p->timeUs;connection=p->proxyConnection;generated|=p->toolGenerated;native|=!p->toolGenerated;}}
            out<<"],\"time_us\":"<<time<<",\"proxy_connection\":"<<connection<<",\"tool_generated\":"<<(generated&&!native?"true":"false")<<",\"mixed_source\":"<<(generated&&native?"true":"false")<<",\"decrypted\":"<<(!decrypted.empty()?"true":"false")<<",\"fields\":";writeFields(out,m);writeConfig(out,m.config);out<<",\"children\":";size_t budget=8*1024*1024;writeExpanded(out,m,0,budget);out<<"}\n";
        }
    }
    out.close();if(!out)throw std::runtime_error("Report write failed");
    std::cout<<"packets="<<packets.size()<<" frames="<<count<<" inbound="<<inboundCount<<" complete_structures="<<complete<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
