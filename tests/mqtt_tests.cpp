#include "mqtt_codec.hpp"
#include "reconnect_backoff.hpp"
#include "query_history.hpp"
#include <iostream>
#include <stdexcept>
using namespace aion;
static void check(bool ok){if(!ok)throw std::runtime_error("MQTT codec assertion failed");}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected);}
int main(){try{
    ReconnectBackoff retry;
    check(retry.failed(0,0)==750);
    check(retry.failed(1000,0)==1500); // Flapping connections do not reset.
    check(retry.failed(29999,0)==3000);
    for(int i=0;i<10000;++i){auto delay=retry.failed(1000,uint32_t(i));check(delay>=6000 && delay<=60000);}
    check(retry.failed(30000,0)==750); // Stable connection resets the next delay.
    ReconnectBackoff left,right;check(left.failed(0,1)!=right.failed(0,250));

    struct HistoryRecord {int64_t expires;};
    QueryHistory<HistoryRecord> history;
    for(int64_t i=0;i<10000;++i){
        const auto now=100000+i*100;
        history.prune(now,"");check(history.ready());
        history.entries.emplace(std::to_string(i),HistoryRecord{now+30000});
        check(history.entries.contains(std::to_string(i))); // Duplicate identity remains reserved.
        check(history.entries.size()<=401);
    }
    history.entries.clear();history.entries.emplace("active",HistoryRecord{100});
    history.entries.emplace("result",HistoryRecord{100});
    history.prune(10100,"active");check(history.entries.size()==2); // Entire grace retained.
    history.prune(10101,"active");check(history.entries.size()==1 && history.entries.contains("active"));
    history.prune(10101,"");check(history.entries.empty());
    for(size_t i=0;i<QueryHistory<HistoryRecord>::Capacity-1;++i)history.entries.emplace(std::to_string(i),HistoryRecord{100});
    check(!history.ready() && !history.full());
    history.entries.emplace("in-flight",HistoryRecord{100});check(history.full());
    history.prune(10101,"");check(history.ready() && history.entries.empty());

    check(mqtt::connect("c","u","p")==Bytes({0x10,19,0,4,'M','Q','T','T',4,0xc2,0,30,0,1,'c',0,1,'u',0,1,'p'}));
    check(mqtt::subscribe(256,"a/b")==Bytes({0x82,8,1,0,0,3,'a','/','b',1}));
    check(mqtt::ack(256)==Bytes({0x40,2,1,0}));
    for(size_t size:{size_t(0),size_t(1),size_t(127),size_t(128),size_t(16000),size_t(32768)}){
        std::string payload(size,'x');if(size>3)payload[2]='\0';
        const auto wire=mqtt::publish(65535,"aion2/query-workers/device/session/task",payload,true);
        mqtt::Decoder decoder;
        for(size_t i=0;i<wire.size();++i){decoder.feed(std::span(wire).subspan(i,1));auto p=decoder.next();
            if(i+1<wire.size())check(!p);else{check(p.has_value());auto publication=mqtt::publication(*p);check(publication.payload==payload && publication.id==65535 && publication.duplicate && !publication.retained);}
        }
        check(!decoder.next());
    }
    mqtt::Decoder combined;combined.feed(Bytes{0xd0,0,0x40,2,0,2});check(combined.next()->header==0xd0);check(combined.next()->body==Bytes({0,2}));check(!combined.next());
    for(const auto& bytes:std::vector<Bytes>{{0xd0,0x80,0},{0x40,0xff,0xff,0xff,0xff,1},{0x01,0},{0xf0,0},{0x81,0}})
        rejects([&]{mqtt::Decoder d;d.feed(bytes);d.next();});
    rejects([]{mqtt::Decoder d;d.feed(Bytes(mqtt::MaxPacket+1));});
    rejects([]{mqtt::publish(0,"a","b");});rejects([]{mqtt::subscribe(1,"a/#");});rejects([]{mqtt::publish(1,"a",std::string(32769,'x'));});
    rejects([]{mqtt::publication({0x36,{0,1,'a',0,1}});});
    rejects([]{mqtt::publication({0x32,{0,5,'a'}});});
    rejects([]{mqtt::publication({0x32,{0,1,'a',0,0}});});
    std::cout<<"MQTT codec tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
