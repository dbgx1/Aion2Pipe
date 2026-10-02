#include "ui.hpp"
#include "self_appear_fixture.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
using namespace aion;
static void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static void put(Bytes& b,uint64_t value,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(value>>(8*i)));}
static Bytes frame(Bytes b){b.insert(b.begin(),uint8_t(b.size()+4));return b;}
static Bytes position(uint8_t key,float x){Bytes b{0x1c,0x37,key,0};for(float v:{x,20.f,30.f})put(b,std::bit_cast<uint32_t>(v),4);put(b,0,4);return frame(b);}
static Packet packet(Bytes data,bool outbound=false,uint64_t connection=1){
    Packet p;p.proxyConnection=connection;p.protocol=6;p.pid=7;p.plaintext=true;p.outbound=outbound;
    p.source.port=outbound?51000:13328;p.destination.port=outbound?13328:51000;p.payload=std::move(data);p.timeUs=1000;return p;
}
namespace aion {
struct AppRetentionTest {
    static NearbyObjects& model(App& app,uint64_t connection=1){return app.nearby_.at(packet({},false,connection).directionKey()+"/0");}
    static void run(){
        App app;
        // Exercise the actual UI ingestion path, with the production 50,000 cap.
        auto appear=packet(selfAppearFixture(0x3fffffff));
        app.add({appear});check(model(app).decoded==1,"proxy frame parsed exactly once");
        std::vector<Packet> batch(49999,packet({}));
        batch.push_back(packet(position(1,40)));app.add(std::move(batch));
        check(app.limit_ && app.packets_.size()==50000,"archive reaches real packet cap");
        check(model(app).objects().at(1).position->at(0)==40,"crossing packet still updates self");
        const auto memory=app.memory_,streamMemory=app.streamMemory_,streamSize=app.streams_.size();
        Bytes tx{0x01,0x37,0};for(float v:{55.f,20.f,30.f})put(tx,std::bit_cast<uint32_t>(v),4);put(tx,0,2);put(tx,0,4);put(tx,1,8);
        auto natural=packet(frame(tx),true);app.add({natural});
        check(model(app).objects().at(1).position->at(0)==55 && app.proxySelfUpdates_==1,"native self motion continues after cap");
        auto generated=natural;generated.toolGenerated=true;
        auto wire=packet(position(1,99));wire.wireObservation=true;
        app.add({generated,wire,packet(position(2,75))});
        check(model(app).objects().at(1).position->at(0)==55 && app.proxySelfUpdates_==1,"wire and generated motion excluded");
        check(model(app).objects().at(2).position->at(0)==75,"new objects still observed");
        app.add({packet(frame({0x42,0x36,2,0,1}))});
        check(!model(app).objects().at(2).present,"departures still observed");
        app.add({packet(selfAppearFixture(0x3fffffff),false,2),packet(position(1,88),false,2)});
        check(model(app,2).objects().at(1).position->at(0)==88 && model(app).objects().at(1).position->at(0)==55,"new connections isolated after cap");
        check(model(app,2).objects().at(1).lastPacket>app.packets_.size(),"unretained update does not link to an unrelated saved packet");
        check(model(app,2).json(false).find("88")!=std::string::npos,"export uses current model");
        auto fin=packet({},false,2);fin.flags=1;app.add({fin});check(model(app,2).closed,"close notification survives archive cap");
        check(app.packets_.size()==50000 && app.memory_==memory && app.streamMemory_==streamMemory && app.streams_.size()==streamSize,"archive remains bounded while objects update");
        Bytes enter{0x21,0x36};enter.insert(enter.end(),46,0);app.add({packet(frame(enter))});
        check(model(app).objects().empty() && model(app).scene==1,"scene transition clears stale objects after cap");
        app.clear();check(app.observedRecords_==0 && app.nearby_.empty() && !app.limit_,"clear resets live and archive state");
        app.mode_="LIVE / test";app.limit_=true;
        app.add({packet(selfAppearFixture(0x3fffffff),false,1)});
        app.followNearby_=false;app.nearbyConnection_=packet({},false,1).directionKey()+"/0";
        for(uint64_t id=2;id<=120;++id){auto closed=packet({},false,id);closed.flags=1;app.add({packet(selfAppearFixture(0x3fffffff),false,id),closed});}
        check(app.nearby_.size()==17 && app.proxyStreamIds_.size()==17,"closed live world history is bounded after archive stops retaining");
        check(!model(app,1).closed && model(app,120).closed,"active world and recent closed snapshot retained");
        app.add({packet(position(1,91),false,1)});check(model(app,1).objects().at(1).position->at(0)==91,"active world updates after repeated reconnects");
        app.clear();app.mode_="EMPTY";
        // The per-stream cap is separate from the total packet/byte caps.
        app.add({appear});auto key=appear.directionKey()+"/0";auto& stream=app.streams_.at(key);
        stream.bytes.resize(Stream::Limit);stream.next=0;app.streamMemory_=Stream::Limit;
        app.add({packet(position(1,65)),packet(position(1,66))});
        check(app.limit_ && model(app).objects().at(1).position->at(0)==66 && !model(app).limited,"8 MiB archive stream cap cannot freeze live objects");
        app.clear();app.add({appear});app.memory_=128*1024*1024;
        app.add({packet(position(1,77)),packet(position(1,78))});
        check(app.limit_ && app.packets_.size()==1 && model(app).objects().at(1).position->at(0)==78,"byte cap still permits later live frames");
    }
};
}
int main(){try{AppRetentionTest::run();std::cout<<"UI retention and live object regression checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
