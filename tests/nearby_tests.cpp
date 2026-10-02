#include "nearby.hpp"
#include "world_probe.hpp"
#include "owner_selection.hpp"
#include "self_appear_fixture.hpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
using namespace aion;
static size_t checks{};
static void check(bool condition,const char* text){++checks;if(!condition)throw std::runtime_error(text);}
static Bytes hexBytes(const char* text){return *parseHex(text);}
static void put(Bytes& b,uint64_t value,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(value>>(i*8)));}
static Bytes frame(const Bytes& body){Bytes b;uint32_t n=uint32_t(body.size()+4);do{auto c=uint8_t(n&127);n>>=7;b.push_back(uint8_t(c|(n?128:0)));}while(n);b.insert(b.end(),body.begin(),body.end());return b;}
static Bytes npc(uint8_t key){
    Bytes b={0x41,0x36,key,0,0,0,0,1,3,'B','o','b',7,0,0,0};
    put(b,0,2);put(b,0x3fc00000,4);put(b,0xc0100000,4);put(b,0x3e800000,4);put(b,0,4);put(b,0,2);
    b.insert(b.end(),{1,100,127});b.insert(b.end(),52,0);b.push_back(0);b.push_back(0);put(b,50,4);b.push_back(0);return frame(b);
}
static Bytes movement(uint8_t key){Bytes b={0x1c,0x37,key,0};put(b,0x41200000,4);put(b,0x41a00000,4);put(b,0x41f00000,4);put(b,0,4);return frame(b);}
static void localMovementTests(){
    struct Fixture {Bytes bytes;size_t bitmap{},angle{};};
    auto fixture=[](unsigned op,uint32_t vessel,unsigned localFlags,unsigned angle){
        Fixture f;auto& b=f.bytes;put(b,op,2);b.push_back(1);
        const bool stopped=op==0x371a,ground=op>=0x371a && op<=0x371d;
        const bool delta=op==0x371d || op==0x3720,attack=op==0x372e || op==0x372f;
        const bool gravityStart=op==0x371e,attackStart=op==0x372e;
        b.push_back(stopped?15:ground?(delta?63:7):attack?(attackStart?127:7):gravityStart?31:delta?15:3);
        if(stopped)b.insert(b.end(),{0,9});
        else if(ground)b.push_back(3); // running + later prediction share this byte
        if(gravityStart)b.push_back(6);
        if(attackStart){put(b,77,4);b.insert(b.end(),{6,9});}
        if(delta)b.insert(b.end(),{1,254,3});
        else {put(b,0x41200000,4);put(b,0x41a00000,4);put(b,0x41f00000,4);}
        if(ground){put(b,16384,2);if(!stopped)put(b,32768,2);}
        else if(!attack)put(b,32768,2);
        if(!ground)for(int i=0;i<3;++i)put(b,0,4);
        if(attack)put(b,0x470cc700,4);
        f.bitmap=b.size();if(delta)b.push_back(uint8_t(localFlags));
        put(b,vessel,4);
        if(delta){for(unsigned i=0;i<3;++i)if(localFlags&(1<<i))b.push_back(i==0?128:i==1?127:255);}
        else {put(b,0x3fc00000,4);put(b,0x40200000,4);put(b,0x40600000,4);}
        if(attack){put(b,0x42c80000,4);put(b,0x43480000,4);put(b,0x43960000,4);}
        if(!ground){put(b,0x3f800000,4);put(b,0xbf800000,4);put(b,0,4);}
        auto var=[&](unsigned value){do{auto byte=uint8_t(value&127);value>>=7;b.push_back(byte|(value?128:0));}while(value);};
        if(ground && !stopped)var(16384);
        f.angle=b.size();var(angle);
        if(stopped){b.push_back(1);put(b,UINT64_MAX,8);}
        else if(gravityStart){b.push_back(3);put(b,UINT64_MAX,8);}
        else if(attackStart){put(b,UINT32_MAX,4);for(int i=0;i<3;++i)put(b,0,4);b.push_back(1);}
        else if(!ground)b.push_back(3);
        return f;
    };
    auto value=[](const GameMessage& m,const char* name){for(const auto& f:m.fields)if(f.name==name)return f.value;return std::string{};};
    for(unsigned op:{0x371a,0x371b,0x371c,0x371d,0x371e,0x371f,0x3720,0x372e,0x372f}){
        const bool delta=op==0x371d || op==0x3720;
        for(unsigned flags=0;flags<(delta?8u:1u);++flags)for(unsigned angle:{0,127,128,16383,16384,65535}){
            auto f=fixture(op,UINT32_MAX,flags,angle);auto m=decodeGameFrame(frame(f.bytes),true);
            check(m.structureComplete && value(m,"局部载体实体编号")=="4294967295","local schema uses a fixed u32 vessel and consumes all outer fields");
            check(value(m,"局部朝向 yaw").starts_with(std::to_string(angle)+" / "),"local yaw accepts all one/two/three-byte u16 boundaries");
            if(delta){if(flags&1)check(value(m,"局部 delta X")=="-128","local X delta is signed");if(flags&2)check(value(m,"局部 delta Y")=="127","local Y delta is signed");if(flags&4)check(value(m,"局部 delta Z")=="-1","local Z delta is signed");}
            else check(value(m,"局部位置 X")=="1.5" && value(m,"X")=="10","local and outer XYZ remain distinct fields");
            if(op==0x371b || op==0x371c || op==0x371d)check(value(m,"跑步状态")=="true" && value(m,"航位推算启用")=="true","bool cursor survives local vectors and varints");
            if(op==0x371e)check(value(m,"向上跳跃")=="true" && value(m,"航位推算启用")=="true" && value(m,"跳板 ID")=="18446744073709551615","gravity optional tail follows the local schema with shared booleans");
            if(op==0x372e || op==0x372f)check(value(m,"局部目标位置 X")=="100" && value(m,"局部移动速度 X")=="1","attack local target precedes velocity");
            if(op==0x372f)check(value(m,"位移结束")=="true","attack move-end bool shares its byte after the local schema");
            for(size_t n=2;n<f.bytes.size();++n)check(!decodeGameFrame(frame(Bytes(f.bytes.begin(),f.bytes.begin()+n)),true).structureComplete,"truncated local movement never accepted");
            NearbyObjects model;model.message(movement(1),1,100);model.message(frame(f.bytes),2,200);
            const auto& o=model.objects().at(1);
            check(o.position==std::array<double,3>{10,20,30} && o.positionPacket==1 && o.positionNeedsRefresh && o.networkPositionNeedsRefresh,"nonzero vessel never substitutes local or outer XYZ for transformed world coordinates");
            check(o.values.at("local_vessel_key")=="4294967295" && model.unparsed==0,"decoded vessel remains visible without labeling the schema incomplete");
            model.message(frame(hexBytes("1d 37 01 02 01 00 00 00 00")),3,300);
            check(o.positionPacket==1 && o.positionNeedsRefresh,"later delta cannot compound a stale world baseline after vessel movement");
            model.message(movement(1),4,400);
            check(!o.positionNeedsRefresh && !o.networkPositionNeedsRefresh && !o.values.contains("local_vessel_key"),"world snapshot restores position and clears obsolete vessel detail");
            auto zero=fixture(op,0,flags,angle);model.message(frame(zero.bytes),5,500);
            check(!o.positionNeedsRefresh && o.positionPacket==5 && o.position==std::array<double,3>{delta?11.:10.,delta?18.:20.,delta?33.:30.},"zero vessel reference keeps the normal outer coordinate path");
        }
        auto bad=fixture(op,1,7,65535);
        bad.bytes[bad.angle+2]=4;
        check(!decodeGameFrame(frame(bad.bytes),true).structureComplete,"local yaw overflow beyond u16 is rejected");
        bad.bytes[bad.angle+2]=128;
        check(!decodeGameFrame(frame(bad.bytes),true).structureComplete,"unbounded local yaw continuation is rejected");
        if(delta){bad=fixture(op,1,8,0);check(!decodeGameFrame(frame(bad.bytes),true).structureComplete,"unknown local bitmap rejected");}
        bad=fixture(op,1,7,0);bad.bytes.push_back(0);
        check(!decodeGameFrame(frame(bad.bytes),true).structureComplete,"local movement trailing bytes rejected");
    }
    NearbyObjects self;self.message(selfAppearFixture(0x3fffffff),1,100);
    self.message(frame(fixture(0x371c,42,0,128).bytes),2,200);
    check(self.objects().at(1).values.contains("local_vessel_key"),"self can retain a server local-coordinate reference");
    Bytes tx={0x01,0x37,0};for(int i=0;i<3;++i)put(tx,0x41200000,4);put(tx,0,2);put(tx,0,4);put(tx,1,8);
    check(self.outbound(frame(tx),3,300) && !self.objects().at(1).values.contains("local_vessel_key") && self.objects().at(1).networkPositionNeedsRefresh,"new nonlocal TX clears stale vessel detail without restoring server baseline");
}
static void serverMovementTests(){
    auto value=[](const GameMessage& m,const char* name){for(const auto& f:m.fields)if(f.name==name)return f.value;return std::string{};};
    auto coordinates=[](Bytes& b){put(b,0x41200000,4);put(b,0x41a00000,4);put(b,0x41f00000,4);};
    for(unsigned op=0x373e;op<=0x3740;++op){
        Bytes b;put(b,op,2);b.insert(b.end(),{1,1,255});coordinates(b);put(b,65535,2);
        auto m=decodeGameFrame(frame(b),true);
        check(m.structureComplete && value(m,"移动状态")=="1 / Run" && value(m,"拒绝原因（枚举待确认）")=="65535","server reject exact layout and u16 reason");
        for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(frame(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"reject truncation cannot become a valid position");
        auto trailing=b;trailing.push_back(0);check(!decodeGameFrame(frame(trailing),true).structureComplete,"reject trailing data stays incomplete");
        NearbyObjects model;model.message(selfAppearFixture(0x3fffffff),1,100);model.message(frame(b),2,200);
        const auto& self=model.objects().at(1);
        check(self.isSelf && self.position==std::array<double,3>{10,20,30} && self.positionSource=="server_correction" && self.positionPacket==2,"server rejection corrects the matching known self");
        check(self.networkPositionNeedsRefresh && !self.networkPosition,"correction does not assume an unverified delta baseline");
        model.message(frame(hexBytes("1d 37 01 02 01 00 00 00 00")),3,300);
        check(self.position==std::array<double,3>{10,20,30} && self.positionPacket==2 && self.positionNeedsRefresh,"delta after correction waits for network snapshot");
        b[2]=2;model.message(frame(b),4,400);
        check(!model.objects().at(2).isSelf && self.positionPacket==2,"server correction uses entity key and never guesses self");
        b[2]=1;b[5]=0;b[6]=0;b[7]=0x80;b[8]=0x7f;model.message(frame(b),5,500);
        check(!self.position && self.positionNeedsRefresh,"nonfinite rejection position is not displayed as valid");
    }
    for(unsigned op=0x371e;op<=0x3720;++op){
        const bool start=op==0x371e,delta=op==0x3720;const unsigned mask=start?31:delta?15:3;
        for(unsigned flags=0;flags<=mask;++flags){
            const bool nested=(flags&(start?2:delta?4:1))!=0;
            Bytes b;put(b,op,2);b.push_back(1);b.push_back(uint8_t(flags));
            if(start && (flags&1))b.push_back(255);
            if(delta){if(flags&1)b.push_back(128);if(flags&2)b.push_back(127);b.push_back(255);}else coordinates(b);
            put(b,32768,2);coordinates(b);
            if(start && (flags&4))b.push_back(3);
            if(start && (flags&8))put(b,UINT64_MAX,8);
            if((flags&(start?16:delta?8:2)) && !(start && (flags&4)))b.push_back(1);
            auto m=decodeGameFrame(frame(b),true);
            if(nested){check(!m.structureComplete,"missing nested gravity data is rejected");continue;}
            check(m.structureComplete && value(m,"朝向 yaw")=="32768 / 180°","gravity flags and u16 yaw decode exactly");
            if(start && (flags&4) && (flags&16))check(value(m,"向上跳跃")=="true" && value(m,"航位推算启用")=="true","gravity bool bits share byte across u64 springboard");
            if(start && (flags&8))check(value(m,"跳板 ID")=="18446744073709551615","gravity springboard preserves full u64");
            for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(frame(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"gravity truncated prefixes are rejected");
            NearbyObjects model;model.message(movement(1),1,100);model.message(frame(b),2,200);
            const auto& o=model.objects().at(1);
            check(o.position==std::array<double,3>{delta && (flags&1)?-118.:10.,delta && (flags&2)?147.:20.,delta?29.:30.},"gravity snapshot or signed optional XY and mandatory Z applied");
            auto bad=b;bad[3]=uint8_t(mask+1);check(!decodeGameFrame(frame(bad),true).structureComplete,"unknown gravity flag rejected");
            b.push_back(0);check(!decodeGameFrame(frame(b),true).structureComplete,"gravity trailing byte rejected");
        }
    }
    for(unsigned op=0x372e;op<=0x372f;++op){
        const bool start=op==0x372e;
        for(unsigned flags=0;flags<=(start?127u:7u);++flags){
            Bytes b;put(b,op,2);b.push_back(1);b.push_back(uint8_t(flags));
            if(start){if(flags&1)put(b,UINT32_MAX,4);if(flags&2)b.push_back(6);if(flags&4)b.push_back(255);}
            coordinates(b);coordinates(b);put(b,0xc2340000,4);
            if(start){if(flags&16)put(b,UINT32_MAX,4);if(flags&32){put(b,0x42c80000,4);put(b,0x43480000,4);put(b,0x43960000,4);}}
            if(start?(flags&64):(flags&6))b.push_back(3);
            auto m=decodeGameFrame(frame(b),true);
            if(flags&(start?8:1)){check(!m.structureComplete,"missing nested attack data is rejected");continue;}
            check(m.structureComplete && value(m,"X")=="10" && value(m,"朝向 yaw")=="-45 / 359.752808°","attack orientation truncates to i32 then uses low u16 angle");
            if(start && (flags&16))check(value(m,"目标实体编号")=="4294967295","attack target is fixed-width u32");
            if(start && (flags&32))check(value(m,"目标位置 X")=="100","target position is distinct from current position");
            if(!start && (flags&6)==6)check(value(m,"航位推算启用")=="true" && value(m,"位移结束")=="true","attack flags share a bool byte");
            for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(frame(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"attack truncated prefixes rejected");
            NearbyObjects model;model.message(selfAppearFixture(0x3fffffff),1,100);model.message(frame(b),2,200);
            const auto& o=model.objects().at(1);
            check(o.position==std::array<double,3>{10,20,30} && o.positionSource=="server_attack" && o.isSelf,"attack updates matching entity from current rather than target XYZ");
            check(!o.networkPositionNeedsRefresh && o.networkPosition==o.position,"attack refreshes native network baseline from current XYZ");
            model.message(frame(hexBytes("1d 37 01 02 01 00 00 00 00")),3,300);
            check(o.position==std::array<double,3>{11,20,30} && o.positionPacket==3 && !o.positionNeedsRefresh,"ordinary delta continues from attack position rather than old appearance or target");
            model.message(frame(hexBytes("20 37 01 00 ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00")),4,400);
            check(o.position==std::array<double,3>{11,20,29} && o.positionPacket==4 && !o.positionNeedsRefresh,"gravity delta shares the attack and ordinary baseline");
            b.push_back(0);check(!decodeGameFrame(frame(b),true).structureComplete,"attack trailing bytes rejected");
        }
    }
    for(unsigned op:{0x372e,0x372f}){
        for(const auto& sample:std::initializer_list<std::pair<uint32_t,const char*>>{
            {0x46143c00,"9487 / 52.1136475°"},{0x470cc700,"36039 / 197.96814°"},
            {0x3fe00000,"1.75 / 0.00549316406°"},{0xbfe00000,"-1.75 / 359.994507°"},
            {0x47800000,"65536 / 0°"},{0xcf000000,"-2.14748365e+09 / 0°"}}){
            Bytes b;put(b,op,2);b.insert(b.end(),{1,0});coordinates(b);coordinates(b);put(b,sample.first,4);
            const auto m=decodeGameFrame(frame(b),true);
            check(m.structureComplete && value(m,"朝向 yaw")==sample.second,"float encoded yaw handles captures, fractions, wrap and i32 minimum");
        }
        for(uint32_t bits:{0x7f800000u,0xff800000u,0x7fc00000u,0x4f000000u,0xcf000001u}){
            Bytes b;put(b,op,2);b.insert(b.end(),{1,0});coordinates(b);coordinates(b);put(b,bits,4);
            check(!decodeGameFrame(frame(b),true).structureComplete,"invalid float encoded yaw fails without an unsafe cast");
            NearbyObjects model;model.message(movement(1),1,100);model.message(frame(b),2,200);
            check(model.objects().at(1).positionPacket==1 && model.objects().at(1).networkPositionNeedsRefresh,"invalid attack yaw cannot advance position or leave a usable stale baseline");
        }
    }
    auto stopped=decodeGameFrame(frame(hexBytes("1a 37 01 0d 0f 07 00 00 20 41 00 00 a0 41 00 00 f0 41 00 80 01 ff ff ff ff ff ff ff ff")),true);
    check(stopped.structureComplete && value(stopped,"移动状态")=="15 / SwimmingDive" && value(stopped,"移动子状态（枚举待确认）")=="7" && value(stopped,"移动惩罚启用")=="true" && value(stopped,"移动惩罚时长（毫秒）")=="18446744073709551615","stop state substate penalty bool and u64 are named without changing wire widths");
    auto running=decodeGameFrame(frame(hexBytes("1c 37 01 05 03 00 00 20 41 00 00 a0 41 00 00 f0 41 00 40 00 80")),true);
    check(running.structureComplete && value(running,"跑步状态")=="true" && value(running,"航位推算启用")=="true" && value(running,"移动方向")=="16384 / 90°","running and prediction share bits across positions and separate direction angles");
    NearbyObjects sprint;
    for(const auto& b:{hexBytes("41 37 ec 0a 00 00"),hexBytes("42 37 ec 0a")}){
        check(decodeGameFrame(frame(b),true).structureComplete,"captured self sprint responses contain no XYZ");
        sprint.message(frame(b),1,100);
    }
    check(sprint.objects().empty(),"sprint inherited key does not manufacture objects or identity");
    NearbyObjects effects;effects.message(selfAppearFixture(0x3fffffff),1,100);
    const auto beforePosition=effects.objects().at(1).position;const auto beforeHp=effects.objects().at(1).values.at("hp");
    effects.message(frame(hexBytes("05 38 01 03 02 03 04 00 00 00 64 05")),2,200);
    check(effects.objects().size()==1 && effects.objects().at(1).lastPacket==2 && effects.objects().at(1).position==beforePosition && effects.objects().at(1).values.at("hp")==beforeHp,"abnormal effect updates receiving entity metadata without inventing caster or treating damage as HP/XYZ");
    effects.message(frame(hexBytes("01 38 03 00 00 4d 00 00 00 01 ff")),3,300);
    check(effects.objects().size()==1 && effects.objects().at(1).lastPacket==2,"skill-start response has no entity key and cannot manufacture an object");
    for(unsigned op:{0x8d02,0x8d03}){
        const auto* fieldName=op==0x8d02?"滑翔冷却（毫秒）":"冲刺冷却（毫秒）";
        const auto* modelName=op==0x8d02?"glide_cooldown_ms":"sprint_cooldown_ms";
        for(uint32_t cooldown:{0u,1u,1000u,UINT32_MAX}){
            Bytes b;put(b,op,2);b.insert(b.end(),{0xff,0xff,0xff,0xff,0x0f});put(b,cooldown,4);
            auto m=decodeGameFrame(frame(b),true);
            check(m.structureComplete && value(m,"实体编号")=="4294967295" && value(m,fieldName)==std::to_string(cooldown),"mobility cooldown is entity ULEB32 plus exact u32 milliseconds");
            for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(frame(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"cooldown truncated key or value rejected");
            NearbyObjects unknown;unknown.message(frame(b),1,100);
            check(unknown.objects().empty(),"local-controller cooldown never invents identity from inherited key");
            NearbyObjects known;known.message(selfAppearFixture(0x3fffffff),1,100);const auto before=known.objects().at(1).position;
            known.message(frame(b),2,200);const auto& self=known.objects().at(1);
            check(known.objects().size()==1 && self.values.at(modelName)==std::to_string(cooldown) && self.position==before && self.positionPacket==1,"cooldown updates known self metadata despite differing ignored key and never XYZ");
            auto stale=b;std::fill(stale.end()-4,stale.end(),uint8_t{0});known.message(frame(stale),1,100);
            check(self.values.at(modelName)==std::to_string(cooldown) && self.lastPacket==2,"old cooldown cannot rewind newer metadata");
            b.push_back(0);check(!decodeGameFrame(frame(b),true).structureComplete,"cooldown trailing bytes rejected");
        }
        NearbyObjects ambiguous;auto first=selfAppearFixture(0x3fffffff);ambiguous.message(first,1,100);
        auto second=first;second[splitGameFrames(second).frames[0].prefixBytes+2]=2;ambiguous.message(second,2,200);
        Bytes incoming;put(incoming,op,2);incoming.push_back(1);put(incoming,1234,4);ambiguous.message(frame(incoming),3,300);
        check(ambiguous.objects().at(1).lastPacket==1 && ambiguous.objects().at(2).lastPacket==2,"ambiguous self identity cannot receive controller-level cooldown updates");
        check(ambiguous.objects().at(1).values.at(modelName)==(op==0x8d02?"10":"20"),"appearance cooldown fields populate self metadata with confirmed millisecond units");
    }
}
static void rotateNotificationTests(){
    auto fixture=[](uint32_t target,uint32_t current,uint8_t reason){Bytes b={0x4e,0x36,1};put(b,target,4);put(b,current,4);b.push_back(reason);return b;};
    auto value=[](const GameMessage& m,const char* name){for(const auto& f:m.fields)if(f.name==name)return f.value;return std::string{};};
    for(unsigned reason:{0u,1u,2u,3u,4u,255u}){
        auto b=fixture(0x42b40000,0xc3340000,uint8_t(reason));auto m=decodeGameFrame(frame(b),true);
        check(m.structureComplete && value(m,"目标朝向（度）")=="90" && value(m,"上报当前朝向（单位待确认）")=="-180","rotate notification contains two distinct raw floats and no XYZ");
        check(value(m,"转向原因").starts_with(std::to_string(reason)+" / "),"rotation reason preserves every wire value");
        for(const auto& f:m.fields)if(f.name=="转向原因")check(f.meaningKnown==(reason<4),"enum sentinel and unknown rotation reasons remain unknown");
        for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(frame(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"truncated rotation never accepted");
        NearbyObjects model;model.message(movement(1),10,100);const auto baseline=model.objects().at(1).networkPosition;
        model.message(frame(b),11,110);const auto& o=model.objects().at(1);
        check(o.values.at("rotate_target_degrees")=="90" && o.values.at("rotate_reported_current")=="-180","rotation metadata applies to matching entity");
        check(o.position==std::array<double,3>{10,20,30} && o.positionPacket==10 && o.networkPosition==baseline && !o.positionNeedsRefresh,"rotation never changes XYZ or network baseline");
        model.message(frame(fixture(0,0,0)),9,90);
        check(o.values.at("rotate_target_degrees")=="90" && o.lastPacket==11,"old rotation cannot rewind metadata");
        b.push_back(0);check(!decodeGameFrame(frame(b),true).structureComplete,"trailing rotation bytes rejected");
    }
    for(uint32_t bits:{0x7fc00000u,0x7f800000u,0xff800000u}){
        NearbyObjects model;model.message(movement(1),1,10);model.message(frame(fixture(bits,bits,0)),2,20);
        const auto& o=model.objects().at(1);check(!o.values.contains("rotate_target_degrees") && !o.values.contains("rotate_reported_current") && o.positionPacket==1,"nonfinite rotation retained in packet only, never object values");
    }
    auto b=fixture(0,0,0);b.erase(b.begin()+2);b.insert(b.begin()+2,{0xff,0xff,0xff,0xff,0x0f});
    check(value(decodeGameFrame(frame(b),true),"实体编号")=="4294967295","rotation key supports full ULEB32");
    b[6]=0x10;check(!decodeGameFrame(frame(b),true).structureComplete,"rotation key overflow rejected");
    NearbyObjects unknown;unknown.message(frame(fixture(0,0,0)),1,10);
    check(unknown.objects().size()==1 && !unknown.objects().at(1).position && !unknown.objects().at(1).isSelf,"rotation alone creates no self identity or coordinates");
    NearbyObjects departed;departed.message(movement(1),1,10);departed.message(frame(hexBytes("47 36 01 00 00")),2,20);departed.message(frame(fixture(0,0,0)),3,30);
    check(!departed.objects().at(1).present && !departed.objects().at(1).values.contains("rotate_target_degrees"),"late rotation does not revive departed identity");
}
static void entityLevelUpdateTests(){
    auto fixture=[](uint32_t key,uint32_t level){Bytes b={0x91,0x8d};do{auto c=uint8_t(key&127);key>>=7;b.push_back(c|(key?128:0));}while(key);put(b,level,4);return b;};
    auto value=[](const GameMessage& m,const char* name){for(const auto& f:m.fields)if(f.name==name)return f.value;return std::string{};};
    for(uint32_t key:{0u,1u,127u,128u,16383u,16384u,UINT32_MAX})for(uint32_t level:{0u,1u,2u,45u,65536u,UINT32_MAX}){
        auto b=fixture(key,level);auto m=decodeGameFrame(frame(b),true);
        check(m.structureComplete && value(m,"实体编号")==std::to_string(key) && value(m,"等级")==std::to_string(level),"level broadcast is ULEB32 key plus fixed u32 level without bitmap");
        for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(frame(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"truncated level broadcast rejected");
        NearbyObjects partial;partial.message(frame(b),1,100);const auto& o=partial.objects().at(key);
        check(o.kind==ObjectKind::Unknown && !o.isSelf && !o.position && o.values.at("level")==std::to_string(level),"level-only object retains unknown identity and absent coordinates");
        b.push_back(0);check(!decodeGameFrame(frame(b),true).structureComplete,"level broadcast rejects trailing bytes");
    }
    NearbyObjects model;model.message(selfAppearFixture(0x3fffffff),1,100);model.message(movement(2),2,200);
    auto& target=model.objects().at(2);const auto position=target.position,network=target.networkPosition;
    const auto selfLevel=model.objects().at(1).values.at("level");
    model.message(frame(fixture(2,88)),3,300);
    check(target.values.at("level")=="88" && model.objects().at(1).values.at("level")==selfLevel,"level applies to the addressed entity rather than local self");
    check(target.position==position && target.networkPosition==network && target.positionPacket==2 && !target.positionNeedsRefresh,"level never changes XYZ or movement baseline");
    model.message(frame(fixture(2,1)),2,200);check(target.values.at("level")=="88" && target.lastPacket==3,"stale level message cannot rewind latest state");
    auto malformed=fixture(2,99);malformed.pop_back();model.message(frame(malformed),4,400);
    check(target.values.at("level")=="88" && target.lastPacket==3 && !target.positionNeedsRefresh,"truncated level cannot mutate metadata or invalidate movement");
    model.message(frame(hexBytes("47 36 02 00 00")),5,500);model.message(frame(fixture(2,99)),6,600);
    check(!target.present && target.values.at("level")=="88","level message cannot revive or update a departed identity");
    auto overflow=fixture(UINT32_MAX,1);overflow[6]=0x10;
    check(!decodeGameFrame(frame(overflow),true).structureComplete,"level entity varint overflow rejected");
}
int main(int argc,char** argv){try{
    entityLevelUpdateTests();
    rotateNotificationTests();
    {Bytes ranking(68,0);ranking[0]=0x57;ranking[1]=0x8d;NearbyObjects model;model.message(frame(ranking),1,10);
     check(model.unparsed==0 && model.objects().empty(),"ranking records do not create world entities or infer self identity");}
    localMovementTests();
    serverMovementTests();
    TcpOwnerSelection owners;bool direction=false;
    owners.add(100,false);owners.add(200,true);
    check(owners.select([](uint32_t p){return p==100;},direction)==100 && direction,"existing loopback target survives reverse proxy owner");
    check(owners.select([](uint32_t p){return p==200;},direction)==200 && !direction,"inbound loopback target keeps its direction");
    check(owners.select([](uint32_t){return true;},direction)==0,"two different target owners remain ambiguous");
    owners.add(300,false);
    check(owners.select([](uint32_t p){return p==200;},direction)==0,"same endpoint conflicting owners are rejected");
    Bytes probe=npc(1);auto probeMove=movement(1);
    check(!findWorldStreamStart(probe),"one appearance cannot identify arbitrary stream");
    probe.insert(probe.end(),probeMove.begin(),probeMove.end());
    check(!findWorldStreamStart(probe),"two messages are insufficient for automatic world recognition");
    probe.insert(probe.end(),probeMove.begin(),probeMove.end());
    check(findWorldStreamStart(probe)==0,"three complete object messages identify plaintext independent of port");
    auto prefixed=frame(Bytes{0x12,0x12,0,0});prefixed.insert(prefixed.end(),probe.begin(),probe.end());
    check(findWorldStreamStart(prefixed)==0,"earlier framed messages are preserved after independent object validation");
    probe.insert(probe.begin(),37,0xAA);
    check(findWorldStreamStart(probe)==37,"mid-frame capture start is skipped only after validated sequence");
    Stream forwarded;forwarded.bytes=probe;NearbyObjects forwardedModel;forwardedModel.consumed=*findWorldStreamStart(probe);forwardedModel.feed(forwarded,1,1);
    check(forwardedModel.objects().size()==1 && (*forwardedModel.objects().at(1).position)[0]==10,"recognized forwarded stream populates nearby objects");
    probe.pop_back();check(!findWorldStreamStart(probe),"truncated third message is not accepted");
    check(!findWorldStreamStart(Bytes(65536,0x16)),"non-world data is rejected within bounded probe");
    const auto clockFrame=frame(hexBytes("00 36 e3 d3 43 eb a0 01 00 00"));
    Bytes heartbeats;for(int i=0;i<3;++i)heartbeats.insert(heartbeats.end(),clockFrame.begin(),clockFrame.end());
    check(!findWorldStreamStart(heartbeats),"clock-only stream cannot identify world without an entity message");
    auto resource=frame(hexBytes("00 8d fa 84 01 02 01 00 b6 41 0f 00 00 00 00 00"));
    resource.insert(resource.end(),heartbeats.begin(),heartbeats.end());
    check(findWorldStreamStart(resource)==0,"resource updates followed by valid clocks recognize stationary world connection");
    NearbyObjects selfModel;selfModel.message(selfAppearFixture(0x3fffffff),1,100);
    const auto& self=selfModel.objects().at(1);
    check(self.isSelf && self.kind==ObjectKind::Player && self.appearanceSeen,"self appearance identifies the controlled player");
    check(self.values.at("level")=="45" && self.values.at("name")=="Self" && self.values.at("map_id")=="101" && self.values.at("channel")=="7","self detail feeds name level and map metadata");
    check(self.position && (*self.position)[0]==1.5 && self.values.at("character_dbid")=="4294967299","self network position and full-width database key retained");
    check(selfModel.json(false).find("\"is_self\": true")!=std::string::npos,"self membership is exported");
    const auto selfPosition=self.position;
    selfModel.message(frame(hexBytes("46 37 e6 1b 00 b6 02 47")),2,200);
    check(selfModel.objects().size()==1 && selfModel.objects().at(1).values.at("last_on_ground_z")=="33462" && self.position==selfPosition,"ground height targets known self without overwriting XYZ or creating wire entity");
    check(self.positionSource=="appearance" && self.positionPacket==1,"self ground-height update does not masquerade as current-position refresh");
    selfModel.message(movement(1),3,300);
    check(self.positionSource=="absolute" && self.positionPacket==3 && (*self.position)[0]==10,"self movement updates position exactly like other entities when matching coordinates are received");
    NearbyObjects noSelf;noSelf.message(frame(hexBytes("46 37 e6 1b 00 b6 02 47")),1,100);
    check(noSelf.objects().empty(),"ground notification alone cannot identify self or manufacture nearby object");
    // New native reader 0x1491E1D50 reads a u16 mask, not the old u32.
    // Derive the minimal wire fixture from the independently specified old one.
    auto oldNpc=npc(1);auto oldSplit=splitGameFrames(oldNpc);
    Bytes newBody(oldNpc.begin()+oldSplit.frames[0].prefixBytes,oldNpc.end());
    newBody.erase(newBody.begin()+5,newBody.begin()+7);
    auto newNpc=frame(newBody);auto parsedNew=decodeGameFrame(newNpc,true);
    check(parsedNew.structureComplete && parsedNew.parsedBytes==newNpc.size(),"new u16 NPC appearance consumes exact frame");
    NearbyObjects newModel;newModel.message(newNpc,1,100);
    check(newModel.objects().at(1).values.at("name")=="Bob" && newModel.objects().at(1).values.at("level")=="50", "new NPC identity and level are aligned");
    check(newModel.objects().at(1).position==std::optional(std::array<double,3>{1.5,-2.25,0.25}),"new NPC position is aligned");
    // Exercise the final two optional fields, including the signed high bit.
    auto tailBody=newBody;tailBody[4]=0xc0;put(tailBody,0x100000001ull,8);put(tailBody,0xfedcba98,4);
    auto tailNpc=frame(tailBody);auto tailDecoded=decodeGameFrame(tailNpc,true);
    check(tailDecoded.structureComplete && tailDecoded.parsedBytes==tailNpc.size(),"new NPC high-bit tail is exactly consumed");
    for(size_t n=2;n<newBody.size();++n)
        check(!decodeGameFrame(frame(Bytes(newBody.begin(),newBody.begin()+n)),true).structureComplete,"truncated new NPC cannot publish a complete object");
    auto extra=newBody;extra.push_back(0xff);
    check(!decodeGameFrame(frame(extra),true).structureComplete,"unrecognized NPC trailing data rejected");
    NearbyObjects model;auto appearance=npc(1);model.message(appearance,1,100);
    check(model.objects().size()==1 && model.objects().at(1).kind==ObjectKind::Npc,"NPC appearance is classified independently of nickname");
    check(model.objects().at(1).values.at("level")=="50" && model.objects().at(1).values.at("name")=="Bob","decoded entity fields populate snapshot");
    check((*model.objects().at(1).position)[1]==-2.25,"float position copied without integer reinterpretation");
    auto move=movement(1);model.message(move,2,200);check((*model.objects().at(1).position)[0]==10,"absolute movement updates existing object");
    model.message(frame(hexBytes("1d 37 01 02 01 00 00 00 00")),3,300);
    check(!model.objects().at(1).positionNeedsRefresh && *model.objects().at(1).position==std::array<double,3>{11,20,30},"signed delta adds directly and omitted axes stay unchanged");
    check(model.objects().at(1).positionSource=="delta" && model.objects().at(1).positionPacket==3,"position provenance is independent of other updates");
    NearbyObjects motion;motion.message(movement(1),1,100);
    motion.message(frame(hexBytes("1d 37 01 0e 80 7f ff 00 00 00 00")),2,200);
    check(*motion.objects().at(1).position==std::array<double,3>{-118,147,29},"signed i8 boundaries -128 and 127 and negative Z apply exactly");
    motion.message(frame(hexBytes("2a 37 01 06 01 ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00")),3,300);
    check(*motion.objects().at(1).position==std::array<double,3>{-117,146,29},"vector-direction delta shares the network position baseline");
    motion.message(frame(hexBytes("1d 37 02 02 01 00 00 00 00")),4,400);
    check(!motion.objects().at(2).position && motion.objects().at(2).positionNeedsRefresh,"delta without absolute baseline does not invent an origin");
    motion.message(frame(hexBytes("1d 37 01 10 00 00 00 00")),5,500);
    check(motion.objects().at(1).positionNeedsRefresh,"unparsed coordinate-system update invalidates existing baseline");
    motion.message(frame(hexBytes("1d 37 01 02 01 00 00 00 00")),6,600);
    check((*motion.objects().at(1).position)[0]==-117 && motion.objects().at(1).positionPacket==3,"later delta never compounds on stale baseline");
    motion.message(movement(1),7,700);
    check(!motion.objects().at(1).positionNeedsRefresh && motion.objects().at(1).positionSource=="absolute","complete absolute message restores the baseline");
    auto invalid=movement(1);invalid[7]=0x80;invalid[8]=0x7f;motion.message(invalid,8,800);
    check(!motion.objects().at(1).position && motion.objects().at(1).positionNeedsRefresh,"non-finite absolute coordinate rejects the entire baseline");
    motion.message(movement(1),9,900);motion.message(frame(hexBytes("ff ff 00")),10,1000);
    check(motion.objects().at(1).positionNeedsRefresh,"undecodable compressed bundle invalidates potentially missed movement");
    motion.message(movement(1),11,1100);auto broken=frame(hexBytes("1d 37 01 02"));motion.message(broken,12,1200);
    check(motion.objects().at(1).positionNeedsRefresh,"truncated delta invalidates its entity baseline");
    model.message(frame(hexBytes("42 36 01 00 01")),4,400);check(!model.objects().at(1).present,"NPC departure removes current membership");
    check(model.json(false)=="[]" && model.json(true).find("Bob")!=std::string::npos,"export follows departed selection");
    model.message(move,5,500);check(!model.objects().at(1).present,"late updates do not resurrect old identity");
    model.message(appearance,6,600);check(model.objects().at(1).present && model.objects().at(1).firstPacket==6 && !model.objects().at(1).positionNeedsRefresh,"reappearance replaces stale snapshot");
    model.message(movement(2),7,700);check(model.objects().at(2).kind==ObjectKind::Unknown && !model.objects().at(2).appearanceSeen,"midstream observed entity remains unclassified");
    auto truncated=appearance;truncated.pop_back();model.message(truncated,8,800);check(model.objects().at(1).lastPacket==6,"incomplete frame cannot change a snapshot");
    // A minimal raw-LZ4 literal block containing two complete entity messages.
    Bytes expanded=npc(3);auto second=npc(4);expanded.insert(expanded.end(),second.begin(),second.end());
    Bytes body={0xff,0xff};put(body,expanded.size(),4);body.push_back(0xf0);size_t left=expanded.size()-15;while(left>=255){body.push_back(255);left-=255;}body.push_back(uint8_t(left));body.insert(body.end(),expanded.begin(),expanded.end());
    model.message(frame(body),9,900);check(model.objects().contains(3) && model.objects().contains(4),"all compressed child appearances are consumed in order");
    auto compressedProbe=frame(body);compressedProbe.insert(compressedProbe.end(),probeMove.begin(),probeMove.end());
    check(findWorldStreamStart(compressedProbe)==0,"compressed child objects contribute to three-message recognition");
    Stream stream;Packet p;p.protocol=6;p.sequence=100;p.flags=2;stream.add(p);NearbyObjects streaming;streaming.feed(stream,10,1000);
    auto joined=npc(7);auto follow=movement(7);joined.insert(joined.end(),follow.begin(),follow.end());
    auto deltaFrame=frame(hexBytes("1d 37 07 02 01 00 00 00 00"));joined.insert(joined.end(),deltaFrame.begin(),deltaFrame.end());
    p.flags=16;p.sequence=106;p.payload=Bytes(joined.begin()+5,joined.end());stream.add(p);streaming.feed(stream,11,1100);
    check(streaming.objects().empty() && streaming.gap,"out-of-order bytes are not interpreted across a gap");
    p.sequence=101;p.payload=Bytes(joined.begin(),joined.begin()+5);stream.add(p);streaming.feed(stream,12,1200);
    check(streaming.objects().size()==1 && (*streaming.objects().at(7).position)[0]==11 && !streaming.gap,"gap closure consumes appearance, absolute and delta exactly once");
    auto updates=streaming.updates;stream.add(p);streaming.feed(stream,13,1300);check(streaming.updates==updates && (*streaming.objects().at(7).position)[0]==11,"TCP retransmission does not replay entity deltas");
    NearbyObjects another;another.message(npc(7),14,1400);check((*another.objects().at(7).position)[0]==1.5 && (*streaming.objects().at(7).position)[0]==11,"same entity key in different connections cannot overwrite");
    p.flags=1;p.payload.clear();stream.add(p);streaming.feed(stream,15,1500);check(streaming.closed,"FIN leaves a labeled historical snapshot");
    Bytes enter={0x21,0x36};enter.insert(enter.end(),46,0);model.message(frame(enter),16,1600);
    check(model.objects().empty() && model.scene==1,"new map preparation clears old scene identities");
    if(argc>1){
        auto packets=loadPcap(argv[1]);std::map<std::string,Stream> streams;std::map<std::string,NearbyObjects> models;std::map<std::string,size_t> generations;
        std::map<ObjectKind,size_t> seen;size_t packet=0;bool databaseId=false,level=false;
        for(const auto& pk:packets){++packet;if(pk.protocol!=6 || pk.source.port!=13328)continue;
            auto base=pk.directionKey();auto& gen=generations[base];auto key=base+"/"+std::to_string(gen);
            if((pk.flags&2) && streams[key].initialized && (streams[key].initialSequence!=pk.sequence || streams[key].closed))key=base+"/"+std::to_string(++gen);
            streams[key].add(pk);auto& snapshot=models[key];snapshot.feed(streams[key],packet,pk.timeUs);
            for(const auto& [id,o]:snapshot.objects()){++seen[o.kind];if(o.kind==ObjectKind::Player){databaseId|=o.values.contains("character_dbid");level|=o.values.contains("level");}}
        }
        check(seen[ObjectKind::Player] && seen[ObjectKind::Npc] && seen[ObjectKind::Environment],"real capture contains all three independently classified kinds");
        check(databaseId && level,"captured players expose database ID and level");
        for(const auto& [key,snapshot]:models){std::cout<<"connection objects="<<snapshot.objects().size()<<" updates="<<snapshot.updates<<" unknown_messages="<<snapshot.unparsed<<"\n";
            if(argc>2){std::ofstream out(argv[2],std::ios::binary);out<<snapshot.json(true);}}
    }
    if(argc>3){
        auto packets=loadPcap(argv[3]);std::map<std::string,Stream> streams;
        for(const auto& packet:packets)if(packet.protocol==6)streams[packet.directionKey()].add(packet);
        size_t worlds=0;
        for(const auto& [key,externalStream]:streams)if(auto start=findWorldStreamStart(externalStream.bytes)){
            ++worlds;NearbyObjects externalModel;externalModel.consumed=*start;externalModel.feed(externalStream,packets.size(),packets.back().timeUs);
            check(*start==0 && externalModel.objects().size()==2,"external accelerated capture identifies one world stream with two objects");
            check(externalModel.unparsed==0 && externalModel.updates==32,"external skill-end and resource notifications decode without unknown frames");
            check(externalModel.objects().contains(17018) && externalModel.objects().contains(24200),"external resource updates preserve both actual entity keys");
            check(externalModel.objects().at(17018).values.at("hp")=="999958" && externalModel.objects().at(24200).values.at("hp")=="1020","external capture preserves final HP after skill-end notifications");
            for(const auto& [id,object]:externalModel.objects())check(object.kind==ObjectKind::Unknown && !object.appearanceSeen && object.values.contains("hp"),"update-only external objects show HP without inventing identity");
        }
        check(worlds==1,"other captured connections are not classified as world");
    }
    if(argc>4){
        auto packets=loadPcap(argv[4]);std::map<std::string,Stream> streams;
        for(const auto& packet:packets)if(packet.protocol==6)streams[packet.directionKey()].add(packet);
        size_t worlds=0;
        for(const auto& [key,externalStream]:streams)if(auto start=findWorldStreamStart(externalStream.bytes)){
            ++worlds;NearbyObjects externalModel;externalModel.consumed=*start;externalModel.feed(externalStream,packets.size(),packets.back().timeUs);
            check(*start==0 && externalModel.objects().size()==5 && externalModel.updates==89 && externalModel.unparsed==0,"second external sample recognizes movement-only world and all five objects");
            size_t positions=0;
            for(auto id:{20418u,23851u,29300u,32694u,40665u}){
                const auto& object=externalModel.objects().at(id);
                check(object.kind==ObjectKind::Unknown && !object.appearanceSeen,"movement-only identities remain unknown without appearance");
                if(object.position && !object.positionNeedsRefresh)++positions;
            }
            check(positions==3,"second external sample has three usable absolute-position baselines");
        }
        check(worlds==1,"second external sample recognizes exactly one world direction");
    }
    if(argc>5){
        auto packets=loadPcap(argv[5]);std::map<std::string,Stream> streams;size_t selves=0,levelUpdates=0;
        for(const auto& packet:packets)if(packet.protocol==6)streams[packet.directionKey()].add(packet);
        for(const auto& [key,externalStream]:streams)if(auto start=findWorldStreamStart(externalStream.bytes)){
            NearbyObjects externalModel;externalModel.consumed=*start;externalModel.feed(externalStream,packets.size(),packets.back().timeUs);
            if(auto target=externalModel.objects().find(2357);target!=externalModel.objects().end()){
                ++levelUpdates;check(target->second.values.at("level")=="2","2222 real 8D91 updates entity 2357 to level 2");
            }
            for(const auto& [id,object]:externalModel.objects())if(object.isSelf){
                ++selves;check(id==1388 && object.position && object.positionSource=="appearance","2222 capture self has only an appearance-position baseline among decoded updates");
                check(std::abs((*object.position)[0]-7860.22754)<0.01 && std::abs((*object.position)[1]+106.203018)<0.01 && (*object.position)[2]==7008,"self uses CharState position, not the separate previous-position field");
                check(object.values.contains("hp"),"other self status updates coexist with unchanged position provenance");
            }
        }
        check(selves==1 && levelUpdates==1,"2222 capture identifies one self and one independently addressed level update");
    }
    std::cout<<checks<<" nearby checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
