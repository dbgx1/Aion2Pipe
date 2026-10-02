#include "game_protocol.hpp"
#include "query_receipt.hpp"
#include "self_appear_fixture.hpp"
#include <iostream>
#include <fstream>
#include <functional>
#include <random>
#include <stdexcept>
using namespace aion;
static int checks;
static void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
static Bytes sample(const char* s){return *parseHex(s);}
static Bytes framed(const Bytes& body){Bytes b;auto n=uint32_t(body.size()+4);do{auto c=uint8_t(n&127);n>>=7;b.push_back(c|(n?128:0));}while(n);b.insert(b.end(),body.begin(),body.end());return b;}
static void skillResponseEffectTests(){
    auto put=[](Bytes& b,uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));};
    auto var=[](Bytes& b,uint64_t v){do{auto c=uint8_t(v&127);v>>=7;b.push_back(c|(v?128:0));}while(v);};
    auto value=[](const GameMessage& m,const char* name){for(const auto& f:m.fields)if(f.name==name)return f.value;return std::string{};};
    for(unsigned flags=0;flags<128;++flags){
        Bytes b={5,0x38};var(b,UINT32_MAX);b.push_back(uint8_t(flags));var(b,128);var(b,UINT32_MAX);put(b,UINT32_MAX,4);
        if(flags&1)var(b,UINT64_MAX);if(flags&2)var(b,0x100000001);
        if(flags&4){var(b,2);put(b,0,4);put(b,UINT32_MAX,4);}
        if(flags&8)put(b,77,4);if(flags&16)var(b,16384);if(flags&32)put(b,88,4);if(flags&64)put(b,99,4);
        auto m=decodeGameFrame(framed(b),true);
        check(m.structureComplete && value(m,"实体编号")=="4294967295" && value(m,"施放者实体编号")=="128" && value(m,"异常状态 UID")=="4294967295","abnormal effect receiver/caster/UID retain separate varints");
        if(flags&1)check(value(m,"剩余伤害（具体用途待确认）")=="18446744073709551615","remaining damage keeps full u64 without signed conversion");
        if(flags&2)check(value(m,"伤害值")=="4294967297","damage keeps more than 32 bits");
        if(flags&4)check(value(m,"屏障 ID[1]")=="4294967295","barrier array elements are fixed u32");
        if(flags&16)check(value(m,"被免疫阻挡的施放者编号")=="16384","immune caster uses varint not fixed u32");
        for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(framed(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"abnormal effect truncated field or optional tail rejected");
        b.push_back(0);check(!decodeGameFrame(framed(b),true).structureComplete,"abnormal effect trailing data rejected");
    }
    for(const auto* hex:{"05 38 01 04 02 03 04 00 00 00 00", "05 38 01 03 02 03 04 00 00 00 00 00"})
        check(decodeGameFrame(framed(sample(hex)),true).structureComplete,"empty barrier array and zero-valued optional damage are valid");
    for(const auto* hex:{"05 38 01 80 02 03 04 00 00 00", "05 38 01 04 02 03 04 00 00 00 7f", "05 38 01 01 02 03 04 00 00 00 ff ff ff ff ff ff ff ff ff 02"})
        check(!decodeGameFrame(framed(sample(hex)),true).structureComplete,"unknown flags, excessive count and u64 overflow rejected");
    for(unsigned flags=0;flags<4;++flags)for(unsigned toggle:{0,1,2,3,255}){
        Bytes b={1,0x38,uint8_t(flags)};put(b,65535,2);put(b,UINT32_MAX,4);if(flags&1)b.push_back(uint8_t(toggle));if(flags&2)b.push_back(255);
        auto m=decodeGameFrame(framed(b),true);
        check(m.structureComplete && value(m,"结果码（0=成功；其他值待确认）")=="65535" && value(m,"技能 ID")=="4294967295","skill response result u16 and skill u32 match runtime reader");
        if(flags&1){bool found=false;for(const auto& f:m.fields)if(f.name=="技能开关变化")found=f.meaningKnown==(toggle<3);check(found,"unknown toggle and Max sentinel remain unconfirmed");}
        if(flags&2)check(value(m,"客户端技能动作 UID")=="255","client action UID remains an independent byte");
        for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(framed(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"skill response truncation rejected");
        b.push_back(0);check(!decodeGameFrame(framed(b),true).structureComplete,"skill response trailing data rejected");
    }
    check(!decodeGameFrame(framed(sample("01 38 04 00 00 01 00 00 00")),true).structureComplete,"skill response unknown bitmap rejected");
}
static void ownSeasonRankingTests(){
    auto put=[](Bytes& b,uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));};
    auto value=[](const GameMessage& m,const char* name){for(const auto& f:m.fields)if(f.name==name)return f.value;return std::string{};};
    auto fixture=[&](unsigned result,unsigned type,unsigned cls){
        Bytes b={0x57,0x8d};put(b,result,2);b.push_back(uint8_t(type));b.push_back(uint8_t(cls));
        put(b,UINT32_MAX,4);put(b,UINT64_MAX,8);b.insert(b.end(),{3,'a','b','c'});b.insert(b.end(),{0});
        put(b,65535,2);put(b,UINT32_MAX,4);put(b,0,4);b.push_back(uint8_t(cls));put(b,UINT64_MAX,8);
        b.insert(b.end(),{0x80,0x01});b.insert(b.end(),128,'x');
        for(int i=0;i<5;++i)put(b,UINT32_MAX,4);put(b,UINT64_MAX,8);return b;
    };
    for(unsigned result:{0u,15936u,65535u})for(unsigned type:{0u,1u,48u,49u,255u})for(unsigned cls:{0u,12u,13u,255u}){
        auto b=fixture(result,type,cls);auto m=decodeGameFrame(framed(b),true);
        check(m.structureComplete,"season ranking consumes full structure even when query result is nonzero");
        check(value(m,"排名条目 角色数据库 ID")=="18446744073709551615" && value(m,"排名条目 公会徽章 ID")=="65535" && value(m,"总排名人数")=="4294967295","ranking exact fixed widths including u16 guild emblem and u64 DBID");
        check(value(m,"排名条目 昵称")=="abc" && value(m,"排名条目 额外展示数据")==std::string(128,'x'),"ranking strings preserve independent byte lengths including two-byte varint");
        for(const auto& f:m.fields){
            if(f.name=="排名玩法类型")check(f.meaningKnown==(type<49),"season enum sentinel and unknown values stay unknown");
            if(f.name=="查询职业类别" || f.name=="排名条目 职业类别")check(f.meaningKnown==(cls<13),"class enum sentinel and unknown values stay unknown");
        }
        b.push_back(0);check(!decodeGameFrame(framed(b),true).structureComplete,"ranking unexpected tail rejected");
    }
    auto b=fixture(0,1,2);
    for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(framed(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"ranking each truncated scalar/string/tail is rejected");
    b[18]=0x7f;check(!decodeGameFrame(framed(b),true).structureComplete,"ranking altered nickname length cannot silently shift remaining schema");
}
static void guildInfoListTests(){
    auto put=[](Bytes& b,uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));};
    auto value=[](const GameMessage& m,const std::string& name){for(const auto& f:m.fields)if(f.name==name)return f.value;return std::string{};};
    auto fixture=[&](unsigned count,unsigned rule,unsigned race){
        Bytes b={0x1d,0x8a,0xff,0xff,uint8_t(count)};
        for(unsigned i=0;i<count;++i){
            put(b,UINT64_MAX,8);b.insert(b.end(),{3,'a','b','c',0,255});
            put(b,UINT64_MAX,8);b.insert(b.end(),{255,255});put(b,65535,2);
            b.insert(b.end(),{0x80,0x01});b.insert(b.end(),128,'x');b.push_back(uint8_t(rule));
            if(i%8==0)b.push_back(0x55);put(b,UINT64_MAX,8);b.push_back(uint8_t(race));
        }return b;
    };
    for(unsigned n:{0u,1u,2u,8u,9u,16u})for(unsigned rule:{0u,1u,2u,3u,4u,255u}){
        auto b=fixture(n,rule,rule);auto m=decodeGameFrame(framed(b),true);
        for(uint8_t op:{uint8_t(7),uint8_t(9)}){auto alias=b;alias[0]=op;auto a=decodeGameFrame(framed(alias),true);check(a.structureComplete && value(a,"公会数量")==std::to_string(n),"recommended/search responses use verified GuildInfo layout");}
        check(m.structureComplete && value(m,"公会数量")==std::to_string(n),"guild entries consume exactly across packed boolean byte boundaries");
        for(unsigned i=0;i<n;++i){auto p="公会["+std::to_string(i)+"]";
            check(value(m,p+" 公会 ID")=="18446744073709551615" && value(m,p+" 徽章 ID")=="65535","guild IDs and emblem retain u64 and u16 widths");
            check(value(m,p+" 公会名称")=="abc" && value(m,p+" 简介")==std::string(128,'x'),"guild variable length UTF8 strings are independent");
            check(value(m,p+" 已申请加入")==((i%2==0)?"true":"false"),"guild boolean cursor is shared across entries");
            for(const auto& f:m.fields)if(f.name==p+" 入会规则" || f.name==p+" 公会种族")check(f.meaningKnown==(rule<4),"guild enum sentinel stays unknown");
        }
        b.push_back(0);check(!decodeGameFrame(framed(b),true).structureComplete,"guild unexpected trailing byte rejected");
    }
    auto b=fixture(9,0,1);
    for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(framed(Bytes(b.begin(),b.begin()+n)),true).structureComplete,"guild list rejects every truncated prefix");
    b=fixture(1,0,1);b[4]=127;check(!decodeGameFrame(framed(b),true).structureComplete,"guild count bounded by available bytes");
    b=fixture(1,0,1);b[13]=127;check(!decodeGameFrame(framed(b),true).structureComplete,"guild malformed string length rejected");
}
static void guildRequestTests(){
    check(encodeGuildRequest(false,0)==sample("07 06 8a 00"),"native guild list request order zero");
    check(encodeGuildRequest(true,0,"Wave")==sample("0b 08 8a 04 57 61 76 65"),"guild search UTF8 length excludes terminator");
    for(const std::string name:{std::string("军团"),std::string(128,'x'),std::string(256,'z')}){
        auto b=encodeGuildRequest(true,0,name);auto f=splitGameFrames(b);auto m=decodeGameFrame(b,true,true);
        check(f.frames.size()==1 && f.consumed==b.size() && m.structureComplete,"guild multibyte strings and multi-byte frame lengths");
        check(m.fields.back().value==name,"guild name preserved");
    }
    bool rejected=false;try{encodeGuildRequest(true,0,"");}catch(const std::invalid_argument&){rejected=true;}check(rejected,"empty guild search rejected");
    rejected=false;try{encodeGuildRequest(false,2);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"enum sentinel rejected");
    rejected=false;try{encodeGuildRequest(true,0,std::string(257,'x'));}catch(const std::invalid_argument&){rejected=true;}check(rejected,"guild input bounded");
}
static void viewCharTests(){
    check(encodeViewCharRequest(1005,282882351594255517ULL)==sample("10 4f 36 ed 03 9d b4 00 00 00 00 ed 03"),"request encoder matches observed native writer after length finalization");
    for(uint32_t server:{1u,65535u})for(uint64_t id:{uint64_t{1},uint64_t{0x100000001},uint64_t{0x7fffffffffffffff}}){
        auto wire=encodeViewCharRequest(server,id);auto parsed=decodeGameFrame(wire,true,true);
        check(wire.size()==13 && parsed.structureComplete && readInteger(wire,3,2,false)==server && readInteger(wire,5,8,false)==id,"query encoding preserves server and DBID boundaries without narrowing");
    }
    for(auto ids:{std::pair<uint32_t,uint64_t>{0,1},{65536,1},{UINT32_MAX,1},{1,0},{1,0x8000000000000000ULL},{1,UINT64_MAX}}){
        bool rejected=false;try{encodeViewCharRequest(ids.first,ids.second);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"query encoder rejects invalid native target identifiers");
    }
    auto put=[](Bytes& b,uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));};
    auto value=[](const GameMessage& m,const char* name){for(const auto& f:m.fields)if(f.name==name)return f.value;return std::string{};};
    for(uint64_t dbid:{uint64_t{0},uint64_t{1},uint64_t{0x100000001},UINT64_MAX}){
        Bytes b={0x4f,0x36,0xff,0xff};put(b,dbid,8);
        auto m=decodeGameFrame(framed(b),true,true);
        check(m.structureComplete && value(m,"目标服务器 ID")=="65535" && value(m,"目标角色数据库 ID")==std::to_string(dbid),"view query uses u16 server and full u64 database ID");
        for(size_t n=2;n<b.size();++n)check(!decodeGameFrame(framed(Bytes(b.begin(),b.begin()+n)),true,true).structureComplete,"view query rejects every truncated prefix");
        check(!decodeGameFrame(framed(b),true).structureComplete && !decodeGameFrame(framed(b),true,true,GameProfile::Login).structureComplete,"view query schema restricted to outbound world traffic");
        b.push_back(0);check(!decodeGameFrame(framed(b),true,true).structureComplete,"view query rejects unexpected tail");
    }
    // Body observed at SendViewChar after the native writer, before frame finalization.
    auto live=decodeGameFrame(framed(sample("4f 36 ed 03 9d b4 00 00 00 00 ed 03")),true,true);
    check(live.structureComplete && value(live,"目标服务器 ID")=="1005" && value(live,"目标角色数据库 ID")=="282882351594255517","live view query bytes agree with entry registers");
    Bytes response={0x50,0x36,0,0,1,3,'a','b','c'};put(response,3001,4);
    put(response,50,4);put(response,5,4);put(response,1234,4);put(response,1005,2);put(response,1006,2);
    response.push_back(1);put(response,42,4);put(response,UINT64_MAX,8);response.push_back(0);put(response,65535,2);
    response.push_back(0);put(response,123,4);put(response,0,8);put(response,0,2);response.push_back(0);
    auto m=decodeGameFrame(framed(response),true);
    check(!m.structureComplete && value(m,"资料角色等级")=="50" && value(m,"资料角色当前服务器 ID")=="1006" && value(m,"资料角色在线")=="true","view response known prefix parsed without claiming unverified equipment tail complete");
    check(value(m,"实体编号").empty() && value(m,"X").empty(),"view response does not fabricate scene key or coordinates");
    QueryReceipt receipt(1006,282882351594255517ULL);
    receipt.complete(m,false);
    check(receipt.snapshot().phase==QueryPhase::Queued,"unsent reply cannot complete queued request");
    receipt.sent();receipt.complete(m,false);
    check(receipt.snapshot().phase==QueryPhase::Completed && receipt.snapshot().presence=="online","verified response prefix completes tracked query");
    receipt.fail("connection_closed");
    check(receipt.snapshot().presence=="online","connection closure preserves completed result");
    QueryReceipt timedOut(1006,1);timedOut.sent();timedOut.fail("timeout");timedOut.complete(m,false);
    check(timedOut.snapshot().phase==QueryPhase::Failed && timedOut.snapshot().presence=="unknown","late success cannot revive expired query");
    QueryReceipt overlap(1006,1);overlap.sent();overlap.complete(m,true);
    check(overlap.snapshot().presence=="unknown" && overlap.snapshot().error=="native_query_overlap","native overlap never attributed to remote query");
    auto offline=response;offline[29]=0;
    QueryReceipt off(1006,1);off.sent();off.complete(decodeGameFrame(framed(offline),true),false);
    check(off.snapshot().phase==QueryPhase::Completed && off.snapshot().presence=="offline","only explicit false flag means offline");
    auto failed=response;failed[2]=1;
    QueryReceipt denied(1006,1);denied.sent();denied.complete(decodeGameFrame(framed(failed),true),false);
    check(denied.snapshot().presence=="unknown" && denied.snapshot().phase==QueryPhase::Failed,"nonzero game result never means offline");
    check(denied.snapshot().error=="game_query_failed:1","game rejection preserves its result code");
    auto regionDenied=response;regionDenied[2]=0x25;regionDenied[3]=0x19;
    QueryReceipt region(1305,1);region.sent();region.complete(decodeGameFrame(framed(regionDenied),true),false);
    check(region.snapshot().error=="game_query_failed:6437" && region.snapshot().presence=="unknown","live game rejection cannot use the default online flag");
    auto shortFailure=decodeGameFrame(framed(Bytes{0x50,0x36,0x25,0x19}),true);
    check(shortFailure.queryResult==6437 && shortFailure.status.find("kCharacter_InvalidServerId")!=std::string::npos,"rejection code survives absent default response tail");
    check(!shortFailure.viewCharPrefixComplete && !shortFailure.skinsComplete,"rejection cannot expose fabricated appearance");
    for(unsigned code:{6432u,6435u,6437u,65535u}){
        auto failure=decodeGameFrame(framed(Bytes{0x50,0x36,uint8_t(code),uint8_t(code>>8)}),true);
        check(failure.queryResult==code && failure.status.find(std::to_string(code))!=std::string::npos,"all rejection codes remain visible");
    }
    check(gameQueryResultText(6435).find("RequetCharacterViewTooSoon")!=std::string::npos,"native rate rejection named without inventing a cooldown");
    check(gameQueryResultText(65535).find("未知")!=std::string::npos,"unknown server result remains unknown");
    check(!decodeGameFrame(framed(Bytes{0x50,0x36,0x25}),true).queryResult,"truncated result cannot produce a fabricated code");
    QueryReceipt missingResult(1305,1);missingResult.sent();missingResult.complete(decodeGameFrame(framed(Bytes{0x50,0x36}),true),false);
    check(missingResult.snapshot().error=="incomplete_response_prefix","missing result is a protocol failure, not a game rejection");
    for(size_t n=2;n<response.size();++n){
        QueryReceipt truncated(1006,1);truncated.sent();truncated.complete(decodeGameFrame(framed(Bytes(response.begin(),response.begin()+n)),true),false);
        check(truncated.snapshot().phase==QueryPhase::Failed,"all truncated prefix responses rejected even with online bit present");
    }
    for(size_t n=2;n<response.size();++n)check(!decodeGameFrame(framed(Bytes(response.begin(),response.begin()+n)),true).structureComplete,"view response truncated prefixes never complete");
}
int main(int argc,char** argv){try {
    viewCharTests();
    guildInfoListTests();guildRequestTests();
    ownSeasonRankingTests();
    skillResponseEffectTests();
    auto time=sample("0e 00 36 e3 d3 43 eb a0 01 00 00");
    auto m=decodeGameFrame(time,true);
    auto specializedBody=sample("00 39 02 ff ff ff ff 03 01 ff ff ff ff 02 00 00 00 00 ff 01 00 00 00 eb 03 00 00 00");
    auto specialized=decodeGameFrame(framed(specializedBody),true);
    check(specialized.structureComplete && specialized.fields[2].value=="2" && specialized.fields[3].value=="4294967295","specialized list handles nested slots followed by empty parent");
    check(specialized.fields[5].value=="1 / Slot_1" && specialized.fields[8].value=="0" && specialized.fields[9].value=="255 / 未知枚举值" && !specialized.fields[9].meaningKnown,"specialized slots retain removal zero and unknown enum bytes");
    check(decodeGameFrame(framed(sample("00 39 00")),true).structureComplete,"empty specialized list is a valid replacement");
    for(size_t n=2;n<specializedBody.size();++n)check(!decodeGameFrame(framed(Bytes(specializedBody.begin(),specializedBody.begin()+n)),true).structureComplete,"specialized nested list rejects all truncated prefixes");
    auto badSpecialized=specializedBody;badSpecialized[7]=127;check(!decodeGameFrame(framed(badSpecialized),true).structureComplete,"specialized slot count is bounded before iterating");
    badSpecialized=specializedBody;badSpecialized[2]=127;check(!decodeGameFrame(framed(badSpecialized),true).structureComplete,"specialized parent count is bounded before iterating");
    badSpecialized=specializedBody;badSpecialized.push_back(0);check(!decodeGameFrame(framed(badSpecialized),true).structureComplete,"specialized list rejects trailing bytes");
    auto skillListBody=sample("00 51 02 0f ff ff ff ff ff fe 01 02 03 04 05 ff ff ff ff 0f ff ff ff ff ff ff ff ff ff 0f 00 eb 03 00 00 01 01 00 00 00 00");
    auto skillList=decodeGameFrame(framed(skillListBody),true);
    check(skillList.structureComplete && skillList.fields[2].value=="2","skill list all optional fields and subsequent minimal entry consume exactly");
    bool maxSkill=false,maxCooldown=false,lastSkill=false,extraFix=false;
    for(const auto& field:skillList.fields){maxSkill|=field.name=="技能 0 技能 ID" && field.value=="4294967295";maxCooldown|=field.name=="技能 0 冷却时间（毫秒）" && field.value=="4294967295" && field.meaningKnown;lastSkill|=field.name=="技能 1 技能 ID" && field.value=="1003";extraFix|=field.name=="技能 0 附加等级 fix" && field.value=="5";}
    check(maxSkill && maxCooldown && lastSkill && extraFix,"skill list u32 limits, additional level order and second-entry boundaries");
    check(decodeGameFrame(framed(sample("00 51 00")),true).structureComplete,"empty skill list is valid");
    for(size_t n=2;n<skillListBody.size();++n)check(!decodeGameFrame(framed(Bytes(skillListBody.begin(),skillListBody.begin()+n)),true).structureComplete,"skill list truncation rejected at every boundary");
    auto invalidSkillList=skillListBody;invalidSkillList[3]=16;check(!decodeGameFrame(framed(invalidSkillList),true).structureComplete,"unknown skill-list bitmap bits rejected");
    invalidSkillList=skillListBody;invalidSkillList[2]=127;check(!decodeGameFrame(framed(invalidSkillList),true).structureComplete,"skill-list count bounded by remaining bytes");
    invalidSkillList=skillListBody;invalidSkillList.push_back(0);check(!decodeGameFrame(framed(invalidSkillList),true).structureComplete,"skill-list trailing bytes rejected");
    auto skillEndBody=sample("06 38 88 bd 01 28 46 0f 00 14 00");
    auto skillEnd=decodeGameFrame(framed(skillEndBody),true);
    check(skillEnd.structureComplete && skillEnd.fields.size()==6 && skillEnd.fields[2].value=="24200" && skillEnd.fields[3].value=="1001000","skill-end wire entity and skill ID");
    check(skillEnd.fields[4].value=="20" && skillEnd.fields[4].size==1 && skillEnd.fields[5].value=="0" && skillEnd.fields[5].size==1 && !skillEnd.fields[5].meaningKnown,"action UID and unconfirmed reason are separate bytes");
    skillEnd=decodeGameFrame(framed(sample("06 38 ff ff ff ff 0f ff ff ff ff ff fe")),true);
    check(skillEnd.structureComplete && skillEnd.fields[2].value=="4294967295" && skillEnd.fields[3].value=="4294967295" && skillEnd.fields[4].value=="255" && skillEnd.fields[5].value=="254","skill-end full-width limits and nonzero reason retained");
    for(size_t n=2;n<skillEndBody.size();++n)check(!decodeGameFrame(framed(Bytes(skillEndBody.begin(),skillEndBody.begin()+n)),true).structureComplete,"skill-end truncated fields rejected");
    skillEndBody.push_back(0);check(!decodeGameFrame(framed(skillEndBody),true).structureComplete,"skill-end trailing bytes rejected");
    auto ground=sample("46 37 e6 1b 00 b6 02 47");
    auto groundMessage=decodeGameFrame(framed(ground),true);
    check(groundMessage.structureComplete && groundMessage.fields.size()==4 && groundMessage.fields[2].value=="3558" && groundMessage.fields[3].value=="33462","ground reference height is an exact entity varint plus float32");
    check(groundMessage.fields[3].type=="float32 LE" && groundMessage.fields[3].offset==5,"ground reference height wire offset and type");
    for(size_t n=2;n<ground.size();++n)check(!decodeGameFrame(framed(Bytes(ground.begin(),ground.begin()+n)),true).structureComplete,"truncated ground reference rejected even with corrected length");
    check(decodeGameFrame(framed(sample("46 37 ff ff ff ff 0f 00 00 80 bf")),true).fields[3].value=="-1","negative ground height and maximum entity key are retained");
    check(!decodeGameFrame(framed(sample("46 37 ff ff ff ff 10 00 00 80 bf")),true).structureComplete,"overflowing entity key rejected");
    ground.push_back(0);check(!decodeGameFrame(framed(ground),true).structureComplete,"ground trailing data remains incomplete");
    for(const auto& body:{sample("46 36 e6 1b 00"),sample("46 36 01 02 ff ff ff ff 0f ff ff ff ff ff ff ff ff ff 01 01 00")}){
        auto message=decodeGameFrame(framed(body),true);check(message.structureComplete,"barrier empty and full-width varint list decoded");
        if(message.fields.size()>4)check(message.fields[4].value=="4294967295" && message.fields[5].value=="18446744073709551615" && message.fields.back().value=="0","barrier UID and remaining value widths preserve unsigned limits and zero");
    }
    for(const auto& body:{sample("46 36 01 01 01 80"),sample("46 36 01 01 01 ff ff ff ff ff ff ff ff ff 02"),sample("46 36 01 81 20")})
        check(!decodeGameFrame(framed(body),true).structureComplete,"barrier truncated, overflowing and excessive lists rejected");
    for(unsigned result=0;result<2;++result){Bytes body=sample("49 36 00 00 01 c1 00 ff ff ff ff 00 00 00 00 01 00 00 00");body[2]=uint8_t(result);auto wire=framed(body);auto message=decodeGameFrame(wire,true);
        check(message.structureComplete && message.fields[4].value=="193 / HPMax" && message.fields[5].value=="-1" && message.fields.back().value=="4294967296","all-stat response preserves result, signed stat and full-width HPMax override");
        for(size_t size=2;size<body.size();++size)check(!decodeGameFrame(framed(Bytes(body.begin(),body.begin()+size)),true).structureComplete,"all-stat response truncated body rejected");
    }
    for(unsigned trial=0;trial<32;++trial){auto mask=trial==31?0x3fffffffu:trial==30?0u:1u<<trial;auto wire=selfAppearFixture(mask);auto self=decodeGameFrame(wire,true);
        check(self.structureComplete && self.parsedBytes==wire.size(),"self appearance optional flags consume exact nested schema");
        auto find=[&](const char* name)->const GameField&{for(const auto& f:self.fields)if(f.name==name)return f;throw std::runtime_error(name);};
        check(find("角色数据库 ID").value=="4294967299" && find("战斗力").value=="4294967299" && find("征服者等级").value=="46","self identity full-width integers and nested levels retained");
        check(selfServerId(self)==2017,"complete self appearance exposes current game server for query routing");
        check(find("附加状态 0 待移除").value=="true" && find("已购买外观定制").value=="true","bool cache survives all nested equipment and effect fields");
        if(mask&0x1000000)check(find("裂缝 PvP 开启").value=="false","late outer flag retains shared bool ordering");
        if(trial==31){size_t prefix=0;while(wire[prefix++]&128){};
            for(size_t size=prefix;size<wire.size();++size){Bytes shortBody(wire.begin()+prefix,wire.begin()+size);check(!decodeGameFrame(framed(shortBody),true).structureComplete,"self appearance truncated body with corrected envelope length rejected");}}
    }
    for(unsigned detail=0;detail<64;++detail)check(decodeGameFrame(selfAppearFixture(0,uint8_t(detail)),true).structureComplete,"every self detail optional mask consumes exact layout");
    check(!decodeGameFrame(selfAppearFixture(0x40000000),true).structureComplete,"undefined self appearance optional bit rejected");
    check(m.structureComplete && m.fields[1].value=="13824","verified runtime opcode");
    check(m.fields[2].value=="1790653486051","verified timestamp");
    check(!decodeGameFrame(time,true,false,GameProfile::Login).structureComplete,"login namespace must not reuse world opcode schema");
    auto camera=sample("19 a1 ff b4 bc f3 a6 33 33 9f c1 4d ba 12 c3 00 00 96 43 97 d7 14");
    m=decodeGameFrame(camera,true,true);
    check(m.structureComplete && m.parsedBytes==22 && m.fields.size()==7,"captured camera message consumes all three angles float and varint");
    check(m.fields[5].value=="300" && m.fields[5].meaningKnown && m.fields[6].value=="338839" && m.fields[6].meaningKnown,"camera zoom and elapsed mouse milliseconds confirmed by reflection and writer");
    check(!decodeGameFrame(camera,true).structureComplete && !decodeGameFrame(camera,true,true,GameProfile::Login).structureComplete,"camera writer schema is restricted to world outbound");
    auto syncRequest=sample("0f 02 36 43 0a ed fd 23 3a 00 00 68");
    m=decodeGameFrame(syncRequest,true,true);
    check(m.structureComplete && m.fields.back().value=="104" && m.fields.back().meaningKnown,"captured time request contains prior RTT");
    check(!decodeGameFrame(syncRequest,true).structureComplete,"time request does not use the different inbound schema");
    m=decodeGameFrame(sample("0e 0c 61 00 04 0a 00 00 00 0e 02"),true);
    check(m.structureComplete && m.fields[4].value=="10" && m.fields[4].meaningKnown && m.fields[5].value=="14" && !m.fields[5].meaningKnown && m.fields[6].value=="2","captured ContentsTicket sample preserves unknown quantity role");
    for(unsigned mask=0;mask<32;++mask){
        Bytes body={0x0c,0x61,uint8_t(mask>>4),uint8_t(mask&15),10,0,0,0};
        for(unsigned bit=0;bit<4;++bit)if(mask&(1u<<bit)){
            auto value=bit<2?sample("00 00 00 00 01 00 00 00"):sample("ff ff ff ff 0f");body.insert(body.end(),value.begin(),value.end());
        }
        body.push_back(1);if(mask&16){auto value=sample("02 00 00 00");body.insert(body.end(),value.begin(),value.end());}
        auto wire=framed(body);m=decodeGameFrame(wire,true);
        check(m.structureComplete && m.parsedBytes==wire.size(),"all ContentsTicket optional field combinations consume exact widths");
        for(const auto& field:m.fields){
            if(field.type=="u64 LE")check(field.value=="4294967296","ticket times preserve high 32 bits");
            if(field.type=="ULEB128" && field.offset>0)check(field.value=="4294967295","ticket counts preserve full unsigned varint range");
        }
        body.pop_back();check(!decodeGameFrame(framed(body),true).structureComplete,"ticket final required or optional field truncation rejected");
    }
    for(auto body:{"a1 ff 00 00 00 00","02 36 00 00 00 00 00 00 00 00 80","02 36 00 00 00 00 00 00 00 00 ff ff ff ff 10"})
        check(!decodeGameFrame(framed(sample(body)),true,true).structureComplete,"outbound camera truncation and timing invalid varints rejected");
    auto loginSelect=sample("0b 0d 39 00 00 00 e1 07");
    for(unsigned role=0;role<6;++role){
        Bytes body={0x57,0x36,uint8_t(role==5?255:role)};auto expiry=sample("00 00 00 00 01 00 00 00");body.insert(body.end(),expiry.begin(),expiry.end());
        m=decodeGameFrame(framed(body),true);
        check(m.structureComplete && m.fields[2].meaningKnown==(role<5) && m.fields[3].value=="4294967296" && m.fields[3].meaningKnown,"mentoring enum and full-width expiry preserve raw unknown enum values");
        body.pop_back();check(!decodeGameFrame(framed(body),true).structureComplete,"mentoring truncated expiry rejected");
    }
    m=decodeGameFrame(sample("0f 57 36 03 00 00 00 00 00 00 00 00"),true);
    check(m.structureComplete && m.fields[2].value=="3 / Mentee_Inactive" && m.fields[3].value=="0","recorded mentoring update carries inactive mentee and unset expiry");
    auto appendProofInteger=[](Bytes& out,uint64_t value,size_t width){for(size_t i=0;i<width;++i)out.push_back(uint8_t(value>>(i*8)));};
    // A minimal entity initialization isolates both common structures. Exercise each
    // optional state bit independently and together, including non-integer vectors.
    for(unsigned trial=0;trial<18;++trial){
        unsigned mask=trial==17?0xffffu:(trial==16?0u:1u<<trial);
        Bytes body=sample("41 36 01 00 00 00 00 1f 03 41 00 42 ff ff ff ff 02 01 02 ff ff ff ff 01");
        appendProofInteger(body,mask,2);
        auto vector=[&](){for(auto v:{0x3fc00000u,0xc0100000u,0x3e800000u})appendProofInteger(body,v,4);};
        vector();appendProofInteger(body,0xc2b40000u,4);appendProofInteger(body,0xffff,2);
        if(mask&1)vector();if(mask&2)appendProofInteger(body,0x3fc00000,4);if(mask&4)appendProofInteger(body,0x8000,2);
        if(mask&8)vector();if(mask&16)vector();if(mask&32)appendProofInteger(body,UINT32_MAX,4);
        body.push_back(255);body.push_back(127);body.push_back(128);body.push_back(1);
        for(unsigned i=0;i<13;++i)appendProofInteger(body,UINT32_MAX-i,4);
        if(mask&64)body.push_back(255);if(mask&128)appendProofInteger(body,UINT32_MAX,4);if(mask&256)body.push_back(255);
        if(mask&512)body.push_back(40);if(mask&1024){auto v=sample("ff ff ff ff 0f");body.insert(body.end(),v.begin(),v.end());}
        if(mask&2048)appendProofInteger(body,UINT32_MAX,4);if(mask&4096)appendProofInteger(body,UINT32_MAX,4);
        if(mask&8192)body.push_back(255);if(mask&16384)appendProofInteger(body,UINT32_MAX,4);if(mask&32768)appendProofInteger(body,UINT32_MAX,4);
        body.insert(body.end(),7,0); // effect count, stat count, fixed u32, tail count
        auto wire=framed(body);auto entity=decodeGameFrame(wire,true);
        check(entity.structureComplete && entity.parsedBytes==wire.size(),"character state optional bits preserve exact wire widths");
        auto field=[&](const char* name)->const auto& {for(const auto& f:entity.fields)if(f.name==name)return f;throw std::runtime_error("missing character proof field");};
        check(field("基础结构 角色昵称").value=="A\\x00B" && field("基础结构 角色昵称").meaningKnown,"character nickname uses verified UTF8 field");
        check(field("基础结构 种族").value=="2 / Dark" && field("基础结构 性别").value=="1 / Male" && field("基础结构 匿名类型").value=="2 / Mercenary" && field("基础结构 覆盖种族").value=="1 / Light","all identity enums match descriptors");
        check(field("状态结构 位置 X（单位待确认）").value=="1.5" && field("状态结构 位置 Y（单位待确认）").value=="-2.25" && field("状态结构 位置 Z（单位待确认）").value=="0.25","state vectors interpret float bits including negative fractional values");
        check(field("状态结构 朝向（单位待确认）").value=="-90" && !field("状态结构 朝向（单位待确认）").meaningKnown && field("状态结构 移动方向（编码待确认）").value=="65535","float direction and u16 movement direction remain distinct with units unclaimed");
        check(field("状态结构 MP").value=="4294967295" && field("状态结构 FP 上限").value=="4294967283" && field("状态结构 FP 上限").meaningKnown,"all thirteen reflected resource slots preserve full u32 range and order");
        if(mask&512){
            auto f=field("状态结构 关系实体类别");check(f.value=="40 / NPC_Dark_Guard" && f.meaningKnown,"relationship enum includes high ordinal");
            auto unknown=wire;unknown[f.offset]=255;auto decoded=decodeGameFrame(unknown,true);bool found=false;
            for(const auto& value:decoded.fields)if(value.name==f.name)found=value.value=="255" && !value.meaningKnown;
            check(decoded.structureComplete && found,"unknown relationship ordinal retained without assigning a meaning");
        }
        if(mask&1024)check(field("状态结构 锁定目标实体键").value=="4294967295" && field("状态结构 锁定目标实体键").type=="ULEB128","lock target retains variable width rather than reflected fixed u32");
        if(mask&8192)check(field("状态结构 翅膀等级").value=="255" && field("状态结构 翅膀等级").size==1,"wing level uses one wire byte");
        for(const auto& f:entity.fields)if(f.name=="基础结构 种族" || f.name=="基础结构 性别" || f.name=="基础结构 匿名类型" || f.name=="基础结构 覆盖种族"){
            auto unknown=wire;unknown[f.offset]=255;auto decoded=decodeGameFrame(unknown,true);bool found=false;
            for(const auto& value:decoded.fields)if(value.name==f.name)found=value.value=="255" && !value.meaningKnown;
            check(decoded.structureComplete && found,"unknown identity enum values remain raw");
        }
        if(trial==17)for(size_t cut=0;cut<body.size();++cut)check(!decodeGameFrame(framed(Bytes(body.begin(),body.begin()+cut)),true).structureComplete,"every truncation in full character state is rejected");
    }
    Bytes passBody=sample("24 e3 05");
    for(unsigned group=0;group<5;++group){
        for(auto value:{UINT32_MAX,0x80000000u,7u})appendProofInteger(passBody,value,4);
        appendProofInteger(passBody,UINT64_MAX,8);appendProofInteger(passBody,uint64_t(1)<<32,8);
        passBody.push_back(2);for(unsigned i=0;i<2;++i)appendProofInteger(passBody,2001,4);
        passBody.push_back(2);for(unsigned i=0;i<2;++i){appendProofInteger(passBody,7,4);passBody.push_back(i?255:128);}
        passBody.push_back(2);for(unsigned i=0;i<2;++i){appendProofInteger(passBody,9,4);appendProofInteger(passBody,UINT32_MAX-i,4);}
        if(group==0 || group==4)passBody.push_back(group==0?0xA5:2);
        appendProofInteger(passBody,UINT32_MAX,4);
    }
    m=decodeGameFrame(framed(passBody),true);
    check(m.structureComplete && m.fields.size()==108 && m.parsedBytes==framed(passBody).size(),"five Battle Pass groups consume all three nested collections and shared bool bytes");
    check(m.fields[3].value=="4294967295" && m.fields[4].value=="2147483648" && m.fields[6].value=="18446744073709551615" && m.fields[7].value=="4294967296","pass header and both timestamps preserve full unsigned wire values");
    check(m.fields[9].value==m.fields[10].value && m.fields[12].value==m.fields[14].value && !m.fields[12].meaningKnown && m.fields[13].value=="128" && m.fields[15].value=="255","pass duplicate keys preserved and level values are fixed u8 rather than packed bool");
    size_t passFlag=0,passFlagByte=0;
    for(const auto& field:m.fields)if(field.type=="bit"){
        if(passFlag%8==0)passFlagByte=field.offset;
        check(field.offset==passFlagByte && field.bit==int(passFlag%8) && field.meaningKnown && field.value==(((passFlag<8?0xA5:2)&(1u<<(passFlag%8)))?"true":"false"),"pass bool bits span counts and outer records before byte rollover");++passFlag;
    }
    check(passFlag==10,"both bools in every pass group observed");
    auto appendProofString=[](Bytes& out,const Bytes& bytes){uint32_t size=uint32_t(bytes.size());do{auto c=uint8_t(size&127);size>>=7;out.push_back(uint8_t(c|(size?128:0)));}while(size);out.insert(out.end(),bytes.begin(),bytes.end());};
    Bytes cardBody=sample("00 e2 05");
    for(unsigned card=0;card<5;++card){
        appendProofInteger(cardBody,UINT64_MAX,8);cardBody.push_back(card==4?255:1);
        appendProofString(cardBody,sample("e4 bd a0 e5 a5 bd"));appendProofInteger(cardBody,uint64_t(1)<<32,8);
        appendProofString(cardBody,card==0?Bytes(130,'x'):Bytes{});appendProofString(cardBody,card==0?sample("c0 af"):sample("41 00 42"));
        if(card==0 || card==4)cardBody.push_back(card==0?0x96:1);
        appendProofInteger(cardBody,UINT64_MAX,8);appendProofInteger(cardBody,uint64_t(1)<<32,8);
    }
    appendProofInteger(cardBody,UINT64_MAX,8);cardBody.push_back(255);cardBody.push_back(128);cardBody.push_back(2);
    for(unsigned i=0;i<2;++i){appendProofString(cardBody,sample("61 63 63"));appendProofInteger(cardBody,UINT64_MAX,8);appendProofString(cardBody,sample("e4 bd a0"));}
    m=decodeGameFrame(framed(cardBody),true);
    check(m.structureComplete && m.parsedBytes==framed(cardBody).size() && m.fields.size()==82,"five message cards and two block entries consume all UTF8 strings times flags and counters");
    check(m.fields[3].value=="18446744073709551615" && m.fields[5].value=="6" && m.fields[7].value=="4294967296" && m.fields[8].value=="130" && m.fields[11].type=="invalid UTF-8 / bytes","card string lengths count bytes not codepoints and malformed UTF8 preserves raw bytes");
    check(!m.fields[56].meaningKnown && m.fields[56].value=="255","unknown message card type preserved");
    size_t cardFlag=0,cardFlagByte=0;
    for(const auto& field:m.fields)if(field.type=="bit"){
        if(cardFlag%8==0)cardFlagByte=field.offset;
        check(field.offset==cardFlagByte && field.bit==int(cardFlag%8) && field.value==(((cardFlag<8?0x96:1)&(1u<<(cardFlag%8)))?"true":"false"),"message-card bool cursor spans variable strings times and card boundaries");++cardFlag;
    }
    check(cardFlag==10,"all card read and lock flags decoded");
    for(const auto& body:{passBody,cardBody}){
        for(size_t cut=3;cut<body.size();++cut){Bytes truncated(body.begin(),body.begin()+cut);check(!decodeGameFrame(framed(truncated),true).structureComplete,"nested pass or message-card truncation rejected at every byte");}
        check(!decodeGameFrame(framed(body),true,true).structureComplete && !decodeGameFrame(framed(body),true,false,GameProfile::Login).structureComplete,"pass and message-card schemas restricted to world inbound");
    }
    check(decodeGameFrame(framed(sample("24 e3 00")),true).structureComplete,"empty pass group list accepted");
    auto emptyCards=sample("00 e2 00 00 00 00 00 00 00 00 00 00 00 00");
    check(decodeGameFrame(framed(emptyCards),true).structureComplete,"empty cards require time both counts and block-list count");
    emptyCards.pop_back();check(!decodeGameFrame(framed(emptyCards),true).structureComplete,"empty cards still require block-list count");
    for(auto body:{"24 e3 ff ff ff ff 0f","00 e2 ff ff ff ff 0f"})check(!decodeGameFrame(framed(sample(body)),true).structureComplete,"huge nested message counts rejected");
    Bytes trustBody=sample("62 e2 02 02");
    for(unsigned i=0;i<2;++i){trustBody.push_back(i?255:2);trustBody.push_back(i?6:3);
        appendProofInteger(trustBody,UINT64_MAX,8);appendProofInteger(trustBody,uint64_t(1)<<32,8);appendProofInteger(trustBody,UINT64_MAX,8);}
    appendProofInteger(trustBody,uint64_t(2)<<32,8);appendProofInteger(trustBody,UINT32_MAX,4);
    m=decodeGameFrame(framed(trustBody),true);
    check(m.structureComplete && m.fields.size()==17 && m.fields[2].value=="false" && m.fields[14].value=="true" && m.fields[2].offset==m.fields[14].offset && m.fields[14].bit==1,"trust flags share bit byte despite intervening map and independent flag values");
    check(m.fields[4].value=="2 / ExchangeKinaLimit" && m.fields[5].value=="3 / TradeBuyLimit" && !m.fields[9].meaningKnown && m.fields[10].value=="6 / Max","trust key and inner type remain separate even when different");
    check(m.fields[6].value=="18446744073709551615" && m.fields[7].value=="4294967296" && m.fields[8].value=="18446744073709551615" && m.fields[15].value=="8589934592" && !m.fields[15].meaningKnown && m.fields[16].value=="4294967295","trust quantities times and stage preserve unsigned widths and unknown update units");
    for(unsigned bits=0;bits<4;++bits){Bytes empty={0x62,0xe2,uint8_t(bits),0};appendProofInteger(empty,0,8);appendProofInteger(empty,0,4);
        m=decodeGameFrame(framed(empty),true);check(m.structureComplete && m.fields.size()==7 && m.fields[2].value==((bits&1)?"true":"false") && m.fields[4].value==((bits&2)?"true":"false"),"empty trust map retains both independent flags and mandatory tail");}
    Bytes adminBody=sample("5c e2 09");
    for(unsigned i=0;i<9;++i){adminBody.push_back(i==8?255:uint8_t(71+i));if(i==0 || i==8)adminBody.push_back(i==0?0xA5:1);}
    m=decodeGameFrame(framed(adminBody),true);
    check(m.structureComplete && m.fields[3].value=="71 / DamageAnalyzer" && m.fields[11].value=="75 / ItemRestore" && m.fields[17].value=="78 / Max" && !m.fields[19].meaningKnown,"admin feature enum differs from control and unlock enum values");
    for(size_t i=0;i<9;++i)check(m.fields[4+i*2].bit==int(i%8) && !m.fields[4+i*2].meaningKnown,"admin bool positions remain shared and polarity unknown");
    Bytes monolithBody=sample("0b e3 02");
    for(unsigned i=0;i<2;++i){appendProofInteger(monolithBody,UINT32_MAX,4);appendProofInteger(monolithBody,i?0x80000000u:UINT32_MAX,4);}
    m=decodeGameFrame(framed(monolithBody),true);check(m.structureComplete && m.fields.size()==7 && m.fields[3].value==m.fields[5].value && m.fields[4].value=="4294967295" && m.fields[6].value=="2147483648" && m.fields[4].meaningKnown,"Monolith preserves repeated identifiers and raw resonance counts rather than derived levels");
    Bytes ignoredBody=sample("23 e2");appendProofInteger(ignoredBody,UINT64_MAX,8);appendProofInteger(ignoredBody,UINT32_MAX,4);
    m=decodeGameFrame(framed(ignoredBody),true);check(m.structureComplete && m.fields.size()==4 && m.fields[2].value=="18446744073709551615" && !m.fields[2].meaningKnown && m.fields[3].value=="4294967295" && !m.fields[3].meaningKnown,"unused notification fixed fields remain semantically unknown");
    for(const auto& body:{trustBody,adminBody,monolithBody,ignoredBody}){
        for(size_t cut=2;cut<body.size();++cut){Bytes truncated(body.begin(),body.begin()+cut);check(!decodeGameFrame(framed(truncated),true).structureComplete,"trust admin Monolith or fixed notification truncation rejected");}
        auto extra=body;extra.push_back(0);check(!decodeGameFrame(framed(extra),true).structureComplete,"new messages reject unexpected trailing bytes");
        check(!decodeGameFrame(framed(body),true,true).structureComplete && !decodeGameFrame(framed(body),true,false,GameProfile::Login).structureComplete,"new trust feature Monolith schemas restricted to world inbound");
    }
    for(auto opcode:{0xE25C,0xE30B}){Bytes body={uint8_t(opcode),uint8_t(opcode>>8),0};check(decodeGameFrame(framed(body),true).structureComplete,"empty admin and Monolith lists accepted");}
    for(auto opcode:{0xE25C,0xE30B,0xE262}){Bytes body={uint8_t(opcode),uint8_t(opcode>>8)};if(opcode==0xE262)body.push_back(0);auto huge=sample("ff ff ff ff 0f");body.insert(body.end(),huge.begin(),huge.end());check(!decodeGameFrame(framed(body),true).structureComplete,"huge new list counts rejected");}
    Bytes controlBody=sample("24 e2 09");
    for(unsigned i=0;i<9;++i){controlBody.push_back(uint8_t(i==8?255:i%4));if(i==0 || i==8)controlBody.push_back(i==0?0x55:1);}
    m=decodeGameFrame(framed(controlBody),true);
    check(m.structureComplete && m.fields.size()==21,"control map consumes nine byte keys and two interspersed bool bytes");
    for(size_t i=0;i<9;++i){const auto& flag=m.fields[4+2*i];check(flag.value==(i%2?"false":"true") && flag.bit==int(i%8) && !flag.meaningKnown,"control polarity stays unknown while shared bit positions decode correctly");}
    check(!m.fields[19].meaningKnown,"unknown control enum retained");
    Bytes periodBody=sample("56 e2 02");unsigned deliveryIndex=0;
    for(unsigned group=0;group<2;++group){
        appendProofInteger(periodBody,UINT32_MAX,4);periodBody.push_back(group?255:6);
        appendProofInteger(periodBody,uint64_t(1)<<32,8);appendProofInteger(periodBody,UINT64_MAX,8);periodBody.push_back(group?6:3);
        for(unsigned j=0;j<(group?6u:3u);++j,++deliveryIndex){
            appendProofInteger(periodBody,UINT32_MAX,4);appendProofInteger(periodBody,j,4);appendProofInteger(periodBody,0x80000000u+j,4);
            if(deliveryIndex==0 || deliveryIndex==8)periodBody.push_back(deliveryIndex==0?0xA5:1);
        }
    }
    m=decodeGameFrame(framed(periodBody),true);
    check(m.structureComplete && m.parsedBytes==framed(periodBody).size() && m.fields.size()==49,"period collection keeps bool cursor across nested and outer list boundaries");
    check(m.fields[4].value=="6 / Tab_06" && m.fields[5].value=="4294967296" && m.fields[6].value=="18446744073709551615" && !m.fields[21].meaningKnown,"period tab enum and full-width times preserved");
    unsigned rewardIndex=0;size_t firstRewardOffset=0;
    for(const auto& field:m.fields)if(field.type=="bit"){
        if(!rewardIndex)firstRewardOffset=field.offset;
        check(field.meaningKnown && field.bit==int(rewardIndex%8) && field.value==(((rewardIndex<8?0xA5:1)&(1u<<(rewardIndex%8)))?"true":"false"),"period rewarded flag uses confirmed SetBit member and wire bits");
        if(rewardIndex<8)check(field.offset==firstRewardOffset,"bool cursor persists through another outer collection header");++rewardIndex;
    }
    check(rewardIndex==9,"all nested reward flags observed");
    Bytes unlockBody=sample("57 e2 04 3e 40 41 ff");m=decodeGameFrame(framed(unlockBody),true);
    check(m.structureComplete && m.fields[3].value=="62 / DamageAnalyzer" && m.fields[4].value=="64 / ChangeCharacter" && m.fields[5].value=="65 / Max" && !m.fields[6].meaningKnown,"unlock contents enum includes full range and preserves unknown values");
    for(const auto& body:{controlBody,periodBody,unlockBody}){
        for(size_t cut=3;cut<body.size();++cut){Bytes truncated(body.begin(),body.begin()+cut);check(!decodeGameFrame(framed(truncated),true).structureComplete,"contents or period truncation rejected");}
        check(!decodeGameFrame(framed(body),true,true).structureComplete && !decodeGameFrame(framed(body),true,false,GameProfile::Login).structureComplete,"contents schemas use world inbound namespace only");
    }
    for(auto opcode:{0xE224,0xE256,0xE257}){
        Bytes body={uint8_t(opcode),uint8_t(opcode>>8),0};check(decodeGameFrame(framed(body),true).structureComplete,"empty contents and period lists accepted");
        body.back()=255;auto huge=sample("ff ff ff 0f");body.insert(body.end(),huge.begin(),huge.end());check(!decodeGameFrame(framed(body),true).structureComplete,"oversized contents counts rejected");
    }
    Bytes arenaBody=sample("2a e3 02");
    for(int i=0;i<2;++i){arenaBody.push_back(uint8_t(i?255:1));appendProofInteger(arenaBody,UINT64_MAX,8);for(int j=0;j<5;++j){auto v=sample("ff ff ff ff 0f");arenaBody.insert(arenaBody.end(),v.begin(),v.end());}}
    m=decodeGameFrame(framed(arenaBody),true);
    check(m.structureComplete && m.fields.size()==17 && m.fields[3].value=="1 / Single" && !m.fields[10].meaningKnown && m.fields[4].value=="18446744073709551615" && m.fields[9].value=="4294967295","arena points and five varint counters preserve widths and unknown match type");
    Bytes subscribeBody=sample("3b e3 02");
    for(auto type:{44,5}){subscribeBody.push_back(uint8_t(type));appendProofInteger(subscribeBody,UINT32_MAX,4);appendProofInteger(subscribeBody,uint64_t(1)<<32,8);appendProofInteger(subscribeBody,UINT64_MAX,8);}
    m=decodeGameFrame(framed(subscribeBody),true);
    check(m.structureComplete && m.fields.size()==11 && m.fields[3].value=="44 / ItemAutoLooting" && m.fields[7].value=="5" && !m.fields[7].meaningKnown && m.fields[6].value=="18446744073709551615","sparse subscription enum keeps undefined gaps unknown and full time values intact");
    Bytes seasonScoreBody=sample("3e e3 02");
    for(auto type:{49,255}){seasonScoreBody.push_back(uint8_t(type));appendProofInteger(seasonScoreBody,UINT32_MAX,4);appendProofInteger(seasonScoreBody,0x80000000u,4);appendProofInteger(seasonScoreBody,UINT64_MAX,8);}
    m=decodeGameFrame(framed(seasonScoreBody),true);
    check(m.structureComplete && m.fields.size()==11 && m.fields[3].value=="49 / Max" && !m.fields[7].meaningKnown && m.fields[4].value=="4294967295" && m.fields[6].value=="18446744073709551615","season nested key fields stay distinct from ranking points");
    Bytes restoreBody=sample("42 e3 02");
    for(int record=0;record<2;++record){
        appendProofInteger(restoreBody,UINT64_MAX,8);appendProofInteger(restoreBody,UINT32_MAX,4);appendProofInteger(restoreBody,uint64_t(1)<<32,8);
        restoreBody.push_back(uint8_t(record?255:2));appendProofInteger(restoreBody,UINT64_MAX,8);appendProofInteger(restoreBody,uint64_t(2)<<32,8);
        restoreBody.push_back(uint8_t(record?0:2));
        if(!record)for(int i=0;i<2;++i){appendProofInteger(restoreBody,17,4);appendProofInteger(restoreBody,UINT64_MAX,8);}
        restoreBody.push_back(uint8_t(record?0:1));if(!record){appendProofInteger(restoreBody,18,4);appendProofInteger(restoreBody,uint64_t(1)<<32,8);}
    }
    m=decodeGameFrame(framed(restoreBody),true);
    check(m.structureComplete && m.fields.size()==25 && m.fields[6].value=="2 / Extraction" && !m.fields[20].meaningKnown && m.fields[10].value==m.fields[12].value,"restorable-item maps preserve duplicate keys and record boundaries");
    check(m.fields[5].value=="4294967296" && m.fields[7].value=="18446744073709551615" && m.fields[8].value=="8589934592" && !m.fields[8].meaningKnown && m.fields[11].value=="18446744073709551615","restore counts dates and unconfirmed expiry units remain exact");
    Bytes restoreStatusBody=sample("43 e3 02");
    for(auto type:{3,255}){appendProofInteger(restoreStatusBody,UINT64_MAX,8);restoreStatusBody.push_back(uint8_t(type));appendProofInteger(restoreStatusBody,UINT32_MAX,4);appendProofInteger(restoreStatusBody,uint64_t(1)<<32,8);}
    m=decodeGameFrame(framed(restoreStatusBody),true);
    check(m.structureComplete && m.fields[3].value=="18446744073709551615" && m.fields[4].value=="3 / AttuneSoulBind" && m.fields[6].value=="4294967296" && !m.fields[8].meaningKnown,"restore status keeps char dbid count timestamp and enum independent");
    for(const auto& body:{arenaBody,subscribeBody,seasonScoreBody,restoreBody,restoreStatusBody}){
        for(size_t cut=3;cut<body.size();++cut){Bytes shorter(body.begin(),body.begin()+cut);check(!decodeGameFrame(framed(shorter),true).structureComplete,"new E3 list record or nested-map truncation rejected");}
        check(!decodeGameFrame(framed(body),true,true).structureComplete && !decodeGameFrame(framed(body),true,false,GameProfile::Login).structureComplete,"new E3 schemas restricted to world inbound");
    }
    for(auto opcode:{0xE32A,0xE33B,0xE33E,0xE342,0xE343}){
        Bytes empty={uint8_t(opcode),uint8_t(opcode>>8),0};check(decodeGameFrame(framed(empty),true).structureComplete,"E3 empty list still consumes its count");
        empty.back()=255;auto huge=sample("ff ff ff 0f");empty.insert(empty.end(),huge.begin(),huge.end());check(!decodeGameFrame(framed(empty),true).structureComplete,"E3 huge counts rejected");
    }
    m=decodeGameFrame(framed(sample("3d e3 ff ff ff ff")),true);check(m.structureComplete && m.fields[2].value=="4294967295" && m.fields[2].meaningKnown,"personal exchange use count is fixed u32");
    check(!decodeGameFrame(framed(sample("3d e3 ff ff ff")),true).structureComplete && !decodeGameFrame(framed(sample("3d e3 00 00 00 00 00")),true).structureComplete,"personal exchange truncation and unexpected tail rejected");
    for(auto op:{0x8D7E,0x8D37,0x8D8C}){
        Bytes body={uint8_t(op),uint8_t(op>>8),2};auto pairs=sample("ff ff ff ff 00 00 00 80 ff ff ff ff 01 00 00 00");
        body.insert(body.end(),pairs.begin(),pairs.end());if(op==0x8D37){auto points=sample("ff ff 00 80");body.insert(body.end(),points.begin(),points.end());}
        m=decodeGameFrame(framed(body),true);
        check(m.structureComplete && m.fields[3].value=="4294967295" && m.fields[4].value=="2147483648" && m.fields[5].value=="4294967295","pair lists preserve separate unsigned u32 values and duplicate keys");
        check(m.fields[3].meaningKnown==(op==0x8D37) && !m.fields[4].meaningKnown,"shared pair layout does not imply shared semantics");
        if(op==0x8D37)check(m.fields[7].value=="65535" && m.fields[8].value=="32768" && !m.fields[7].meaningKnown,"both skill points are independent unsigned u16 values with unresolved roles");
        for(size_t cut=3;cut<body.size();++cut){Bytes shorter(body.begin(),body.begin()+cut);check(!decodeGameFrame(framed(shorter),true).structureComplete,"pair record or trailing skill point truncation rejected");}
        check(!decodeGameFrame(framed(body),true,true).structureComplete && !decodeGameFrame(framed(body),true,false,GameProfile::Login).structureComplete,"pair schemas restricted to world inbound");
    }
    check(decodeGameFrame(framed(sample("7e 8d 00")),true).structureComplete,"empty pair list supported");
    check(decodeGameFrame(framed(sample("37 8d 00 01 00 02 00")),true).structureComplete,"empty skill list still requires both point values");
    check(!decodeGameFrame(framed(sample("37 8d 00")),true).structureComplete,"missing points after empty skill list rejected");
    check(decodeGameFrame(framed(sample("8c 8d 00")),true).structureComplete,"empty second pair list supported independently");
    auto ceilingBody=sample("12 57 02 ff ff ff ff fe fd ff ff ff ff 01 02");
    m=decodeGameFrame(framed(ceilingBody),true);
    check(m.structureComplete && m.fields.size()==9 && m.fields[3].value=="4294967295" && m.fields[3].meaningKnown && m.fields[4].value=="254" && m.fields[5].value=="253" && !m.fields[4].meaningKnown,"ceiling list preserves two independent bytes and known Dungeon key");
    for(size_t cut=3;cut<ceilingBody.size();++cut){Bytes shorter(ceilingBody.begin(),ceilingBody.begin()+cut);check(!decodeGameFrame(framed(shorter),true).structureComplete,"ceiling record truncation rejected");}
    check(decodeGameFrame(framed(sample("12 57 00")),true).structureComplete,"empty ceiling list supported");
    Bytes questBody=sample("09 8d 02 ff ff ff ff 02");auto questMax=sample("ff ff ff ff 0f");
    for(int i=0;i<7;++i)questBody.insert(questBody.end(),questMax.begin(),questMax.end());
    auto questUid=sample("ff ff ff ff");questBody.insert(questBody.end(),questUid.begin(),questUid.end());questBody.insert(questBody.end(),questMax.begin(),questMax.end());
    auto questReward=sample("00 00 00 80 01 ff ff ff ff ff ff ff ff 0f ff ff ff ff ff ff ff ff ff");questBody.insert(questBody.end(),questReward.begin(),questReward.end());
    Bytes emptyQuestRecord(22,0);emptyQuestRecord[4]=4;questBody.insert(questBody.end(),emptyQuestRecord.begin(),emptyQuestRecord.end());
    m=decodeGameFrame(framed(questBody),true);
    check(m.structureComplete && m.fields.size()==33 && m.fields[4].value=="2 / Incomplete" && m.fields[21].value=="4 / Rewarded","quest records and nested rewards remain aligned across records");
    check(m.fields[5].value=="4294967295" && m.fields[11].value=="4294967295" && m.fields[12].value=="4294967295" && m.fields[14].value=="2147483648","quest seven varints UID and publisher retain full-width values");
    check(m.fields[17].value=="4294967295" && m.fields[17].meaningKnown && m.fields[18].value=="255" && m.fields[18].meaningKnown && m.fields[19].value=="18446744073709551615" && !m.fields[19].meaningKnown,"reward quantity enchantment and unscaled probability use verified widths");
    for(size_t cut=3;cut<questBody.size();++cut){Bytes shorter(questBody.begin(),questBody.begin()+cut);check(!decodeGameFrame(framed(shorter),true).structureComplete,"quest and reward truncations rejected at every position");}
    for(unsigned status=0;status<7;++status){Bytes body={0x09,0x8d,1};body.insert(body.end(),emptyQuestRecord.begin(),emptyQuestRecord.end());body[7]=uint8_t(status==6?255:status);
        m=decodeGameFrame(framed(body),true);check(m.structureComplete && m.fields[4].meaningKnown==(status<6),"quest status enum preserves unknown values without inventing a meaning");}
    for(auto body:{"09 8d 00","8c 8d 00"})check(decodeGameFrame(framed(sample(body)),true).structureComplete,"empty verified list schemas accepted");
    for(auto body:{"09 8d ff ff ff ff 0f","8c 8d ff ff ff ff 0f","12 57 ff ff ff ff 0f"})check(!decodeGameFrame(framed(sample(body)),true).structureComplete,"new list counts bounded before allocation");
    Bytes dutyBody=sample("1c 8d 01 00 00 00 02 00 00 00 01 03 00 00 00 04 00 00 00 05 00 00 00 06 00 00 00 fe 01 ff ff ff ff ff ff ff ff 0f fd ff ff ff ff ff ff ff ff 00 00 00 00 01 00 00 00 02 07 00 00 00 07 00 00 00 00 00 00 00 02 00 00 00");
    m=decodeGameFrame(framed(dutyBody),true);
    check(m.structureComplete && m.fields.size()==20 && m.fields[11].value=="4294967295" && m.fields[11].meaningKnown,"nested duty record identifies only proven Item configuration key");
    check(m.fields[12].value=="4294967295" && m.fields[12].meaningKnown && m.fields[13].value=="253" && m.fields[13].meaningKnown && m.fields[14].value=="18446744073709551615" && !m.fields[14].meaningKnown,"duty reward count enchantment and raw probability retain all bits with proven semantics");
    check(m.fields[15].value=="4294967296" && m.fields[19].value=="8589934592" && m.fields[17].value==m.fields[18].value,"duty times retain high bits and additional IDs retain duplicates");
    for(size_t cut=2;cut<dutyBody.size();++cut){Bytes shorter(dutyBody.begin(),dutyBody.begin()+cut);check(!decodeGameFrame(framed(shorter),true).structureComplete,"all nested duty truncations rejected");}
    check(decodeGameFrame(framed(sample("1c 8d 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00")),true).structureComplete,"empty duty lists keep required scalar and time fields");
    for(auto body:{"7e 8d ff ff ff ff 0f","37 8d ff ff ff ff 0f","1c 8d 00 00 00 00 00 00 00 00 ff ff ff ff 0f"})
        check(!decodeGameFrame(framed(sample(body)),true).structureComplete,"oversized record counts rejected without allocation");
    Bytes seasonBody=sample("31 93 02");
    auto seasonRecord=sample("fe ff ff ff ff 01 00 00 00 02 00 00 00 03 00 00 00 fd 00 00 00 00 01 00 00 00 04 00 00 00 05 00 00 00 00 00 00 00 02 00 00 00 ff ff ff ff ff ff ff ff");
    check(seasonRecord.size()==50,"season record fixture matches independent wire-width sum");
    seasonBody.insert(seasonBody.end(),seasonRecord.begin(),seasonRecord.end());seasonBody.insert(seasonBody.end(),seasonRecord.begin(),seasonRecord.end());
    m=decodeGameFrame(framed(seasonBody),true);
    check(m.structureComplete && m.fields.size()==25 && m.fields[4].value=="4294967295" && m.fields[4].meaningKnown && m.fields[15].value=="4294967295","two 50-byte season records preserve duplicate schedule-group keys");
    check(m.fields[3].value=="254" && !m.fields[3].meaningKnown && m.fields[8].value=="253" && m.fields[9].value=="4294967296" && m.fields[12].value=="8589934592" && m.fields[13].value=="18446744073709551615","season ignored byte and all three u64 values remain untruncated");
    for(size_t cut=3;cut<seasonBody.size();++cut){Bytes shorter(seasonBody.begin(),seasonBody.begin()+cut);check(!decodeGameFrame(framed(shorter),true).structureComplete,"season records cannot accept any truncated body");}
    check(decodeGameFrame(framed(sample("31 93 00")),true).structureComplete,"season empty list supported");
    check(!decodeGameFrame(framed(sample("31 93 ff ff ff ff 0f")),true).structureComplete,"season overcount rejected");
    check(!decodeGameFrame(framed(sample("31 93 00 00")),true).structureComplete,"season unexpected tail remains incomplete");
    for(unsigned mask=0;mask<16;++mask){
        Bytes body={0x0b,0x61,2};
        for(unsigned record=0;record<2;++record){body.push_back(uint8_t(mask));auto key=sample("ff ff ff ff");body.insert(body.end(),key.begin(),key.end());
            for(unsigned bit=0;bit<4;++bit)if(mask&(1u<<bit)){auto value=bit<2?sample("00 00 00 00 01 00 00 00"):sample("ff ff ff ff 0f");body.insert(body.end(),value.begin(),value.end());}}
        auto wire=framed(body);m=decodeGameFrame(wire,true);
        check(m.structureComplete && m.parsedBytes==wire.size() && m.fields[2].value=="2","ticket list optional fields remain aligned across duplicate-key records");
        for(const auto& f:m.fields)if(f.type=="u64 LE")check(f.value=="4294967296" && !f.meaningKnown,"ticket list retains high timestamp bits and unknown roles");
        body.pop_back();check(!decodeGameFrame(framed(body),true).structureComplete,"second ticket record truncation rejected");
    }
    for(unsigned mask=0;mask<8;++mask){
        Bytes body={0x03,0x61,2};
        for(unsigned record=0;record<2;++record){body.push_back(uint8_t(mask));auto key=sample("a1 bb 0d 00");body.insert(body.end(),key.begin(),key.end());
            if(mask&1)body.push_back(255);for(unsigned bit=1;bit<3;++bit)if(mask&(1u<<bit)){auto value=sample("ff ff ff ff 0f");body.insert(body.end(),value.begin(),value.end());}}
        auto wire=framed(body);m=decodeGameFrame(wire,true);
        check(m.structureComplete && m.parsedBytes==wire.size() && m.fields[4].value=="900001" && m.fields[4].meaningKnown,"all dungeon optional combinations and duplicate keys parse");
        if(mask&1)check(m.fields[5].value=="255" && !m.fields[5].meaningKnown,"dungeon byte remains raw despite consumer increment");
        body.pop_back();check(!decodeGameFrame(framed(body),true).structureComplete,"second dungeon record truncation rejected");
    }
    m=decodeGameFrame(sample("19 03 61 02 07 a1 bb 0d 00 01 55 c8 47 07 a2 bb 0d 00 01 22 be 3d"),true);
    check(m.structureComplete && m.fields[6].value=="85" && m.fields[7].value=="9160" && m.fields[11].value=="34" && m.fields[12].value=="7870","captured two dungeon records retain count and time field order");
    m=decodeGameFrame(framed(sample("32 92 03 ff ff ff ff 01 00 00 00 ff ff ff ff")),true);
    check(m.structureComplete && m.fields.size()==6 && m.fields[3].value=="4294967295" && m.fields[3].meaningKnown && m.fields[5].value==m.fields[3].value,"teleport list retains u32 width order and duplicate keys");
    auto mapping=sample("30 93 02 01 00 00 00 ff 00 00 00 00 01 00 00 00 01 00 00 00 ff ff ff ff ff ff ff ff ff");
    m=decodeGameFrame(framed(mapping),true);
    check(m.structureComplete && m.fields.size()==9 && m.fields[5].value=="4294967296" && m.fields[8].value=="18446744073709551615","nonempty classification mapping retains full u64 values and duplicate keys");
    for(size_t i=3;i<m.fields.size();++i)check(!m.fields[i].meaningKnown,"classification mapping does not invent semantics from empty captured sample");
    for(auto body:{"0b 61 00","03 61 00","32 92 00","30 93 00"})check(decodeGameFrame(framed(sample(body)),true).structureComplete,"all four empty lists supported");
    for(auto body:{"0b 61 ff ff ff ff 0f","03 61 01 00 01 00 00","32 92 01 01 00 00","30 93 01 01 00 00 00 00 01 00 00 00","30 93 00 01","03 61 01 04 01 00 00 00 80"})
        check(!decodeGameFrame(framed(sample(body)),true).structureComplete,"list overcounts scalar and varint truncation and extra bytes rejected");
    m=decodeGameFrame(loginSelect,true,false,GameProfile::Login);
    check(m.structureComplete && m.fields.back().value=="2017","login select-server response layout");
    check(!decodeGameFrame(loginSelect,true).structureComplete,"world namespace must not reuse login schema");
    auto sdk=framed(sample("5a e2 00 00 03 aa bb cc"));
    m=decodeGameFrame(sdk,true);
    check(m.structureComplete && m.fields.back().size==3 && !m.fields.back().meaningKnown,"SDK envelope retains opaque inner data without claiming field meanings");
    sdk=framed(sample("5a e2 00 00 04 aa bb cc"));
    check(!decodeGameFrame(sdk,true).structureComplete,"SDK byte count cannot exceed remaining payload");
    sdk=framed(sample("5a e2 01 00 00"));
    check(decodeGameFrame(sdk,true).structureComplete,"SDK error response with empty blob");
    sdk=framed(sample("5a e2 00 00 01 aa bb"));
    check(!decodeGameFrame(sdk,true).structureComplete,"SDK trailing byte remains unresolved");
    auto potion=framed(sample("48 56 02 01 01 00 00 00 02 00 00 00 02 03 00 00 00 04 00 00 00"));
    m=decodeGameFrame(potion,true);
    check(m.structureComplete && m.fields[4].meaningKnown && m.fields[5].meaningKnown && !m.fields[7].meaningKnown && !m.fields[8].meaningKnown,"potion candidate semantics apply only to the proven category");
    check(m.fields[4].value=="1" && m.fields[5].value=="2","primary and fallback potion IDs preserve field order");
    m=decodeGameFrame(framed(sample("4b 56 02 01 00 00 00 ff ff ff ff")),true);
    check(m.structureComplete && m.fields.back().value=="4294967295" && m.fields.back().meaningKnown,"auto-use Item IDs retain unsigned width");
    check(decodeGameFrame(framed(sample("48 56 00")),true).structureComplete && decodeGameFrame(framed(sample("4b 56 00")),true).structureComplete,"empty auto-use lists supported");
    check(!decodeGameFrame(framed(sample("48 56 01 01 01 00 00 00 02 00 00")),true).structureComplete,"potion tuple truncation rejected");
    check(!decodeGameFrame(framed(sample("4b 56 ff ff ff ff 0f")),true).structureComplete,"auto-use count cannot overrun payload");
    m=decodeGameFrame(framed(sample("72 56 01 ff ff ff ff 07")),true);
    check(m.structureComplete && m.fields[3].value=="4294967295" && m.fields[3].meaningKnown && !m.fields[4].meaningKnown,"wing key known while auxiliary byte stays unresolved");
    m=decodeGameFrame(framed(sample("79 56 01 ff ff ff ff 01 02 03 00 00 00")),true);
    check(m.structureComplete && m.fields[3].meaningKnown && !m.fields.back().meaningKnown && m.fields.back().value=="3","title acquired set is distinct from unresolved association table");
    m=decodeGameFrame(framed(sample("82 56 ff ff 01 00")),true);
    check(m.structureComplete && m.fields[2].value=="65535" && !m.fields[2].meaningKnown,"achievement auxiliary values keep u16 width and unresolved meaning");
    auto achievement=framed(sample("83 56 01 01 00 00 00 ff ff ff ff 00 00 00 00 01 00 00 00 02 02 03 00 00 00 03 a5 04 00 00 00 06 01"));
    m=decodeGameFrame(achievement,true);size_t achievementBits=0;size_t achievementSharedAt=0;
    for(const auto& f:m.fields)if(f.bit>=0){if(achievementBits==0)achievementSharedAt=f.offset;
        check(f.offset==(achievementBits<8?achievementSharedAt:achievementSharedAt+6) && f.bit==int(achievementBits%8) && f.value==(((achievementBits<8?0xa5:1)&(1<<(achievementBits%8)))?"true":"false") && !f.meaningKnown,"achievement bits span groups despite intervening key and count");++achievementBits;}
    check(m.structureComplete && achievementBits==9 && m.fields[5].value=="4294967296","achievement time remains full u64 and both maps consume frame");
    check(decodeGameFrame(framed(sample("83 56 00 02 01 00 00 00 01 03 02 00 00 00 01")),true).structureComplete,"last achievement group can consume a cached bit with no remaining bytes");
    for(auto body:{"72 56 00","79 56 00 00","83 56 00 00","83 56 00 01 01 00 00 00 00"})
        check(decodeGameFrame(framed(sample(body)),true).structureComplete,"empty wing title achievement collections supported");
    for(auto body:{"72 56 01 01 00 00 00","79 56 00 01 02 01 00 00","82 56 01 00 02","83 56 00 01 01 00 00 00 09 01","83 56 00 01 01 00 00 00 ff ff ff ff 0f","72 56 ff ff ff ff 0f","79 56 00 00 01"})
        check(!decodeGameFrame(framed(sample(body)),true).structureComplete,"collection truncation overflow and trailing bytes rejected");
    Bytes oversizedAchievement=sample("83 56 00 09");
    for(unsigned i=0;i<9;++i){auto group=sample("01 00 00 00 80 20");oversizedAchievement.insert(oversizedAchievement.end(),group.begin(),group.end());oversizedAchievement.insert(oversizedAchievement.end(),512,0);}
    m=decodeGameFrame(framed(oversizedAchievement),true);
    check(!m.structureComplete && m.status=="字段数量超过限制" && m.fields.size()==32768,"field budget exhaustion returns partial result without throwing again while adding raw tail");
    m=decodeGameFrame(framed(sample("33 56 02 ff ff ff ff ff ff ff ff 02 00 00 00 00 00 00 80")),true);
    check(m.structureComplete && m.fields[3].value=="4294967295" && m.fields[3].meaningKnown && m.fields[4].value=="-1" && m.fields.back().value=="-2147483648","GodstoneData keys are unsigned and held values sign extend as client does");
    check(!m.fields[4].meaningKnown,"GodstoneData table identity does not prove held-value business meaning");
    m=decodeGameFrame(framed(sample("1d 56 ff ff ff ff 01 00 00 00")),true);
    check(m.structureComplete && m.fields[2].value=="4294967295" && m.fields[3].value=="1" && !m.fields[3].meaningKnown,"equipment-level pair retains width and unknown second role");
    m=decodeGameFrame(framed(sample("ac 56 04 01 01 80 ff")),true);
    check(m.structureComplete && m.fields.size()==7 && m.fields[3].value==m.fields[4].value && m.fields.back().value=="255" && m.fields.back().meaningKnown,"opened fog-group IDs preserve duplicates and unsigned byte width on wire");
    auto accountBody=sample("b6 56 02");
    auto accountEntry=sample("ff 01 00 00 00 02 00 00 00 ff ff ff ff ff ff ff ff ff ff 03 00 00 00 04 00 00 00");
    accountBody.insert(accountBody.end(),accountEntry.begin(),accountEntry.end());accountBody.insert(accountBody.end(),accountEntry.begin(),accountEntry.end());
    m=decodeGameFrame(framed(accountBody),true);
    check(m.structureComplete && m.fields.size()==17 && m.fields[4].meaningKnown && m.fields[5].meaningKnown && !m.fields[7].meaningKnown,"nonempty account records retain both schedule lookup keys with unknown value roles");
    check(m.fields[6].value=="65535" && m.fields[7].value=="18446744073709551615" && m.fields[8].value=="3" && m.fields[9].value=="4" && m.fields[10].offset==31,"27-byte account record has exact scalar order and widths across entries");
    accountBody.pop_back();check(!decodeGameFrame(framed(accountBody),true).structureComplete,"account record final scalar truncation rejected");
    for(auto body:{"33 56 00","ac 56 00","b6 56 00"})check(decodeGameFrame(framed(sample(body)),true).structureComplete,"new empty lists parse completely");
    for(auto body:{"33 56 01 00 00 00 00","1d 56 01 00 00 00","ac 56 02 01","b6 56 ff ff ff ff 0f","ac 56 00 01"})
        check(!decodeGameFrame(framed(sample(body)),true).structureComplete,"new schemas reject truncated oversized and trailing data");
    Bytes enterMapBody=sample("21 36 01 00 00 00 56 04 00 00 00 00 00 00 01 00 00 00 00 00 80 bf 00 00 00 40 00 00 40 40 00 00 b4 42 02 03 ff ff ff ff ff ff ff ff 01 01 00 00 00 04 00 00 00 00 01 00 00 00 ac 02 03 e4 b8 ad 05");
    m=decodeGameFrame(framed(enterMapBody),true);
    check(m.structureComplete && m.fields[3].value=="1110" && m.fields[3].meaningKnown && m.fields[4].value=="4294967296","map preparation keeps Map key and u64 timing input");
    check(m.fields[5].value=="-1" && m.fields[6].value=="2" && m.fields[7].value=="3" && m.fields[8].value=="90" && !m.fields[8].meaningKnown,"map preparation distinguishes position floats from unconfirmed rotation component");
    check(m.fields[11].value=="18446744073709551615" && m.fields[15].value=="4294967296" && m.fields[16].value=="300" && m.fields[18].value=="中" && m.fields[19].value=="5","map preparation variable list string and tail align after nonempty entry");
    enterMapBody.pop_back();check(!decodeGameFrame(framed(enterMapBody),true).structureComplete,"map preparation missing final byte rejected");
    auto loadMapBody=sample("23 36 00 00 00 00 00 00 00 00 80 bf 00 00 00 40 00 00 40 40 05 00 00 00 e4 0c");
    m=decodeGameFrame(framed(loadMapBody),true);
    check(m.structureComplete && m.fields[4].value=="-1" && m.fields[5].value=="2" && m.fields[6].value=="3" && m.fields[8].value=="1636 / 客户端保存值 0" && !m.fields[8].meaningKnown,"load-map coordinates and encoded varint preserve evidence without inventing its role");
    loadMapBody[2]=1;check(decodeGameFrame(framed(loadMapBody),true).structureComplete,"load-map error still carries full scalar structure");
    loadMapBody.pop_back();check(!decodeGameFrame(framed(loadMapBody),true).structureComplete,"load-map incomplete final varint rejected");
    m=decodeGameFrame(framed(sample("4d 8d 01 00 ff ff ff ff ff ff ff ff")),true);
    check(m.structureComplete && m.fields.back().value=="18446744073709551615" && m.fields.back().meaningKnown,"item result includes full u64 instance ID even on failure");
    check(!decodeGameFrame(framed(sample("4d 8d 00 00 ff ff ff ff ff ff ff")),true).structureComplete,"truncated item instance ID rejected");
    for(auto op:{0x07,0x08}){auto action=sample("07 95 ff ff ff ff 0f ff ff ff ff ff ff ff ff ff");action[0]=uint8_t(op);
        m=decodeGameFrame(framed(action),true);
        check(m.structureComplete && m.fields[2].value=="4294967295" && m.fields[3].value=="4294967295" && m.fields[4].value=="255" && m.fields[5].value=="4294967295","social action varint and fixed-width fields retain full unsigned range");
        check(m.fields[2].meaningKnown && m.fields[3].meaningKnown && !m.fields[4].meaningKnown && m.fields[5].meaningKnown,"social action state enum remains unresolved");
        action.pop_back();check(!decodeGameFrame(framed(action),true).structureComplete,"stop action still requires all serialized fields");}
    m=decodeGameFrame(framed(sample("a8 56 01 00 ff ff ff ff 01 00 00 00")),true);
    check(m.structureComplete && m.fields[3].value=="4294967295" && m.fields[4].value=="1" && !m.fields[3].meaningKnown,"customizing error response retains both unknown u32 fields");
    Bytes community=sample("74 e2 09");
    for(unsigned i=0;i<9;++i){auto entry=sample("01 46 00 ff ff ff ff ff ff ff ff 03 e4 b8 ad 02 03 04 00 00 00 05 00 00 00 06");
        community.insert(community.end(),entry.begin(),entry.end());if(i%8==0)community.push_back(i==0?0xa5:1);
        auto tail=sample("00 00 00 00 01 00 00 00 01 41");community.insert(community.end(),tail.begin(),tail.end());}
    auto communityB=sample("01 5a 00 00 00 00 00 01 00 00 00 01 42");community.insert(community.end(),communityB.begin(),communityB.end());
    m=decodeGameFrame(framed(community),true);size_t communityBits=0,communityFirstBit=0,communityNames=0;
    for(const auto& f:m.fields){if(f.bit>=0){if(!communityBits)communityFirstBit=f.offset;
        check(f.bit==int(communityBits%8) && (communityBits>=8?f.offset>communityFirstBit:f.offset==communityFirstBit) && f.value==(((communityBits<8?0xa5:1)&(1<<(communityBits%8)))?"true":"false"),"community bool cache crosses strings scalars and entries");++communityBits;}
        if(f.value=="中")++communityNames;}
    check(m.structureComplete && communityBits==9 && communityNames==9 && m.fields.back().value=="B","both nonempty community lists and optional UTF8 strings align");
    community.pop_back();check(!decodeGameFrame(framed(community),true).structureComplete,"community second-list string truncation rejected");
    check(decodeGameFrame(framed(sample("74 e2 00 00")),true).structureComplete,"empty community lists supported");
    for(auto body:{"74 e2 ff ff ff ff 0f","74 e2 00 01 01 00","74 e2 00 00 01","a8 56 00 00 00 00 00 00"})
        check(!decodeGameFrame(framed(sample(body)),true).structureComplete,"community and customizing truncation overflow and trailing bytes rejected");
    m=decodeGameFrame(framed(sample("2d 57 01 ff ff ff ff ff ff ff ff 80 01 00 00 00 02 00 00 00 00 00 00 00 01 00 00 00 ff ff ff ff ff ff ff ff")),true);
    check(m.structureComplete && m.fields.size()==9 && m.fields[3].value=="18446744073709551615" && m.fields[7].value=="4294967296" && m.fields[8].value=="18446744073709551615","nonempty special distribution record preserves six fields and u64 widths");
    Bytes rewardBody=sample("0f 57 01 00 09");
    for(unsigned i=0;i<9;++i){auto part=sample("ff ff ff ff ff ff ff ff 01 00 00 00 02 00 00 00 00 01 00 00 00 03 00 00 00");
        rewardBody.insert(rewardBody.end(),part.begin(),part.end());if(i%8==0)rewardBody.push_back(i==0?0xa5:1);rewardBody.push_back(0x80);}
    m=decodeGameFrame(framed(rewardBody),true);size_t rewardBits=0,rewardBoolAt=0;
    for(const auto& f:m.fields)if(f.bit>=0){if(!rewardBits)rewardBoolAt=f.offset;
        check(f.bit==int(rewardBits%8) && (rewardBits<8?f.offset==rewardBoolAt:f.offset>rewardBoolAt) && f.value==(((rewardBits<8?0xa5:1)&(1<<(rewardBits%8)))?"true":"false"),"reward boolean cache survives fixed fields and per-entry trailing byte");++rewardBits;}
    check(m.structureComplete && rewardBits==9 && m.fields.back().value=="128","reward error result does not remove serialized array");
    rewardBody.pop_back();check(!decodeGameFrame(framed(rewardBody),true).structureComplete,"reward final byte truncation rejected");
    m=decodeGameFrame(framed(sample("45 8a 01 00 ff ff ff ff ff ff ff ff ff ff ff ff")),true);
    check(m.structureComplete && m.fields[3].value=="4294967295" && m.fields[4].value=="18446744073709551615","guild response retains u32 and u64 fields on failure");
    Bytes additionalMail=sample("49 e3 01 00 00 00 00 00 01 00 00 00 01 02 ff ff ff ff 01 03 e4 b8 ad 01 41 01 42 00 00 00 00 01 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff 00 00 00 00 01 00 00 00");
    m=decodeGameFrame(framed(additionalMail),true);
    check(m.structureComplete && m.fields[3].value=="4294967296" && m.fields[6].value=="-1" && m.fields[7].value=="1","mail composite key low term follows raw movsxd rather than unsigned concatenation");
    check(m.fields[9].value=="中" && m.fields[11].value=="A" && m.fields[13].value=="B" && m.fields[16].meaningKnown && m.fields.back().value=="4294967296","additional mail strings Item key and full u64 auxiliary field preserved");
    additionalMail.pop_back();check(!decodeGameFrame(framed(additionalMail),true).structureComplete,"mail upper half of final u64 cannot be discarded as client does");
    for(auto body:{"2d 57 00","0f 57 00 00 00","49 e3 00 00 00 00 00 00 00 00 00 00 00","4e e3 00 00","4e e3 ff ff"})
        check(decodeGameFrame(framed(sample(body)),true).structureComplete,"empty and status-only response structures supported");
    for(auto body:{"2d 57 01","0f 57 00 00 ff ff ff ff 0f","45 8a 00 00 00 00 00 00","49 e3 00 00 00 00 00 00 00 00 00 00 01","4e e3 00","4e e3 00 00 00"})
        check(!decodeGameFrame(framed(sample(body)),true).structureComplete,"remaining outer schemas reject overflow truncation and trailing bytes");
    auto setting=framed(sample("44 8d a5 ff ff ff ff 02 03 0b 01"));
    m=decodeGameFrame(setting,true);size_t settingBits=0;
    for(const auto& f:m.fields)if(f.bit>=0){
        const auto byte=settingBits<8?0xa5:0x0b;const auto bit=settingBits%8;
        check(f.offset==(settingBits<8?3:10) && f.bit==int(bit) && f.value==((byte&(1<<bit))?"true":"false"),"settings bit cursor survives intervening scalars and refills at ninth bit");++settingBits;
    }
    check(m.structureComplete && settingBits==12 && m.fields[4].value=="-1","settings signed sentinel and all twelve bits parsed");
    check(m.fields[3].name=="启用备用药水选择" && m.fields[3].meaningKnown && m.fields[3].bit==1,"fallback option mapped to the second shared bit");
    auto classification=framed(sample("53 8d 01 02 03 e4 b8 ad 01 00 00 00 00 02 00 00 00 01 41 03 00 00 00"));
    m=decodeGameFrame(classification,true);
    check(m.structureComplete && m.fields[5].value=="中" && m.fields.back().value=="3","classification map has three interleaved UTF8 strings and fixed-width numbers");
    Bytes progressBody=sample("58 8d 09");
    for(unsigned i=0;i<9;++i){auto entry=sample("01 02 ff ff ff ff 64 00 00 00 01 00 00 00 00 00 00 00 00");
        progressBody.insert(progressBody.end(),entry.begin(),entry.end());if(i==0)progressBody.push_back(0x55);if(i==8)progressBody.push_back(1);
        progressBody.insert(progressBody.end(),8,0);
    }
    m=decodeGameFrame(framed(progressBody),true);size_t progressBits=0;size_t sharedAt=0;
    for(const auto& f:m.fields)if(f.bit>=0){if(progressBits==0)sharedAt=f.offset;
        check(f.offset==(progressBits<8?sharedAt:sharedAt+217) && f.bit==int(progressBits%8) && f.value==(progressBits%2==0?"true":"false"),"progress boolean byte spans array entries and refills on entry nine");++progressBits;
    }
    check(m.structureComplete && progressBits==9,"progress array with signed ratio fields and empty strings fully consumed");
    progressBody.pop_back();check(!decodeGameFrame(framed(progressBody),true).structureComplete,"progress final integer truncation rejected");
    check(!decodeGameFrame(framed(sample("53 8d ff ff ff ff 0f")),true).structureComplete,"classification oversized count rejected");
    check(!decodeGameFrame(framed(sample("44 8d a5 ff ff ff ff 02 03")),true).structureComplete,"settings second shared bit byte required");
    auto utf=sample("13 0f 39 00 00 01 00 06 e4 b8 ad e6 96 87 10 34");
    m=decodeGameFrame(utf,true,false,GameProfile::Login);
    check(m.structureComplete && m.fields[5].type=="UTF-8" && m.fields[5].value=="中文","UTF-8 byte length preserves multibyte characters");
    utf[8]=0xc0;m=decodeGameFrame(utf,true,false,GameProfile::Login);
    check(m.structureComplete && m.fields[5].type=="invalid UTF-8 / bytes","invalid UTF-8 retained as bytes without corrupting report encoding");
    auto pair=time;pair.insert(pair.end(),time.begin(),time.end());
    check(splitGameFrames(pair).frames.size()==2,"coalesced frames");
    pair.pop_back();check(splitGameFrames(pair).consumed==time.size(),"partial second frame");
    Bytes large(128,0);large[0]=130;large[1]=1;large[2]=0xff;large[3]=0xff;
    auto s=splitGameFrames(large);check(s.consumed==128 && s.frames[0].prefixBytes==2,"multi-byte length correction");
    check(splitGameFrames(Bytes{0x80}).status=="等待完整长度","partial varint");
    check(splitGameFrames(Bytes{0x80,0x80,0x80,0x80}).frames.empty(),"overlong length");
    check(splitGameFrames(Bytes{0}).frames.empty(),"underflow");
    check(splitGameFrames(Bytes{5,0}).frames.empty(),"missing opcode");
    check(splitGameFrames(large,127).frames.empty(),"frame cap");
    m=decodeGameFrame(time,false);check(!m.structureComplete && m.fields.size()==2,"outbound not decoded as plaintext");
    m=decodeGameFrame(sample("1b 1c 37 cc 82 01 04 55 a9 a1 c7 7e 19 e6 c7 00 3d 04 47 31 dc 31 dc 01"),true);
    check(m.structureComplete && m.fields[2].value=="16716","live position frame");
    m=decodeGameFrame(sample("11 1d 37 dd 1a 2f 03 6e 9f f7 7d e2 7d e2"),true);
    check(m.structureComplete,"live delta frame consumes exactly");
    check(m.fields[6].value=="-97" && m.fields[7].value=="-9","signed coordinate deltas");
    check(m.fields[4].offset==6 && m.fields.back().offset==6 && m.fields.back().bit==1,"noncontiguous packed booleans reuse byte");
    m=decodeGameFrame(sample("0d 1c 37 01 00 00 00 00 00 00"),true);
    check(!m.structureComplete && m.status=="字段截断","truncated known structure");
    m=decodeGameFrame(sample("07 99 99 01"),true);
    check(!m.structureComplete && m.parsedBytes==3 && m.fields.back().name=="未解析字节","unknown bytes preserved");
    m=decodeGameFrame(sample("0e 00 8d 01 01 01 00 ff ff ff ff"),true);
    check(m.structureComplete && m.fields.back().value=="-1" && m.fields.back().name.find("HP")!=std::string::npos,"resource values sign extend as in client handler");
    m=decodeGameFrame(sample("16 4a 36 01 01 c1 00 ff ff ff ff 00 00 00 00 01 00 00 00"),true);
    check(m.structureComplete && m.fields[4].value=="193 / HPMax" && m.fields.back().value=="4294967296","HPMax retains distinct 64-bit override beyond uint32");
    auto power=sample("16 56 36 00 00 00 00 01 00 00 00 ff ff ff ff ff ff ff ff");
    m=decodeGameFrame(power,true);
    check(m.structureComplete && m.fields[2].value=="4294967296" && m.fields[3].value=="18446744073709551615","combat-power values preserve full u64 width");
    check(!m.fields[2].meaningKnown && !m.fields[3].meaningKnown,"combat-power event does not establish roles of its two values");
    power.pop_back();--power[0];check(!decodeGameFrame(power,true).structureComplete,"truncated combat-power second value rejected");
    auto limits=sample("16 a9 ff 00 00 00 00 01 00 00 00 ff ff ff ff ff ff ff ff");
    m=decodeGameFrame(limits,true);
    check(m.structureComplete && m.fields[2].value=="4294967296" && m.fields[3].value=="18446744073709551615","abyss limits preserve both u64 fields");
    check(!m.fields[2].meaningKnown && !m.fields[3].meaningKnown,"abyss event does not prove individual value semantics");
    limits.pop_back();--limits[0];check(!decodeGameFrame(limits,true).structureComplete,"truncated abyss limit rejected");
    auto states=sample("18 84 56 01 ff ff ff ff 78 56 34 12 00 00 00 00 01 00 00 00 ff");
    m=decodeGameFrame(states,true);
    check(m.structureComplete && m.fields[3].value=="4294967295" && m.fields[5].value=="4294967296" && m.fields[6].value=="255","status map preserves distinct keys time and byte without narrowing");
    states[3]=2;check(!decodeGameFrame(states,true).structureComplete,"status map count cannot overrun its body");
    check(decodeGameFrame(sample("07 84 56 00"),true).structureComplete,"empty status map accepted");
    auto remove=sample("16 2c 38 01 01 07 ac 02 0b 90 03 ff ff ff ff 01 00 00 00");
    m=decodeGameFrame(remove,true);
    check(m.structureComplete && m.fields[5].value=="300" && m.fields[7].value=="400" && m.fields[7].meaningKnown,"abnormal removal optional entity and full-width values");
    remove[8]=0;m=decodeGameFrame(remove,true);check(m.structureComplete && m.fields[6].value=="0 / None" && m.fields[7].meaningKnown,"reflection names dispel field independently of whether the consumer uses it for that reason");
    remove[8]=255;m=decodeGameFrame(remove,true);check(m.structureComplete && m.fields[6].value=="255" && !m.fields[6].meaningKnown,"unknown abnormal expire reason preserved");
    remove[5]=8;check(!decodeGameFrame(remove,true).structureComplete,"unknown removal option bits rejected");
    auto handshake=sample("19 11 36 00 00 01 00 00 00 00 02 00 00 00 03 00 00 00 f8 ff ff ff");
    m=decodeGameFrame(handshake,true);
    check(m.structureComplete && m.fields[5].size==0 && m.fields.back().value=="-8","world handshake empty blob and signed hour offset");
    handshake[9]=0x7f;check(!decodeGameFrame(handshake,true).structureComplete,"world handshake blob length cannot exceed remaining bytes");
    auto emptyLogin=sample("21 15 36 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 15 00 00 00 02 03 04");
    m=decodeGameFrame(emptyLogin,true);check(m.structureComplete,"empty world login list with trailing strings and flags");
    size_t loginBits=0;for(const auto& field:m.fields)if(field.type=="bit"){
        check(field.offset==23 && field.bit==int(loginBits) && field.value==((loginBits%2==0)?"true":"false"),"world login booleans share one byte across strings and integers");++loginBits;}
    check(loginBits==5,"world login contains five shared booleans");
    auto trade=sample("0b 63 e2 03 46 00 5a 00");m=decodeGameFrame(trade,true);
    check(m.structureComplete && m.fields[2].offset==3 && m.fields[3].offset==3 && m.fields[2].bit==0 && m.fields[3].bit==1,"trade booleans share exactly one byte");
    check(m.fields[4].value=="70" && m.fields[5].value=="90" && m.fields[4].meaningKnown && m.fields[5].meaningKnown,"trade server IDs mapped to HTTP game_server_id consumers");
    trade.pop_back();--trade[0];check(!decodeGameFrame(trade,true).structureComplete,"truncated trade server ID rejected");
    Bytes mail=sample("15 e2 00 00 01 00 00 00 00 00 00 00 00 05");
    auto append=[](Bytes& b,uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));};
    std::vector<size_t> mailBitPositions;
    for(size_t i=0;i<5;++i){mail.push_back(1);mail.push_back(2);append(mail,1234,4);append(mail,0x100000000ULL+i,8);
        mail.push_back(3);mail.insert(mail.end(),{0xe4,0xb8,0xad});
        if(i%4==0){mailBitPositions.push_back(mail.size());mail.push_back(0x55);}
        append(mail,0x100000000ULL,8);append(mail,UINT64_MAX,8);mail.push_back(1);
        append(mail,42,4);append(mail,UINT64_MAX,8);append(mail,0x100000000ULL,8);append(mail,0xabcd,2);
    }
    auto pointState=sample("2e e2 04 01 00 80 02 ff 7f 05 ff ff ff 00 00");
    m=decodeGameFrame(framed(pointState),true);
    check(m.structureComplete && m.fields[4].value=="-32768" && m.fields[6].value=="32767" && m.fields[8].value=="-1","Daevanion point values use client signed 16-bit interpretation");
    check(m.fields[3].value=="1 / DaevanionCrystal" && m.fields[7].value=="5 / Season03Crystal01" && m.fields[3].meaningKnown && !m.fields[9].meaningKnown,"verified Daevanion enum names do not assign a name to unknown values");
    check(!m.fields[4].meaningKnown && m.fields[4].type=="i16 LE","point adjunct field keeps unresolved business meaning");
    pointState.pop_back();check(!decodeGameFrame(framed(pointState),true).structureComplete,"point state partial i16 rejected");
    auto nodes=sample("26 e2 02 ff ff ff ff 02 ff ff ff ff 01 00 00 00 ff ff ff ff 00");
    m=decodeGameFrame(framed(nodes),true);
    check(m.structureComplete && !m.fields[3].meaningKnown && m.fields[5].meaningKnown && m.fields[5].value=="4294967295","Daevanion node configuration IDs distinguished from unresolved group keys");
    check(m.fields[3].value==m.fields[7].value && m.fields.back().value=="0","duplicate Daevanion groups and empty inner list retained");
    nodes.back()=1;check(!decodeGameFrame(framed(nodes),true).structureComplete,"Daevanion inner count cannot overrun remaining bytes");
    Bytes bossRecords=sample("37 e2 02");
    for(uint32_t row=0;row<2;++row){bossRecords.push_back(0xfe);
        for(auto value:{11u,12u,0xffffffffu,0xdeadbeefu,15u})append(bossRecords,value,4);
        append(bossRecords,UINT64_MAX-row,8);append(bossRecords,16,4);append(bossRecords,17,4);
        append(bossRecords,row?0:0x100000001ULL,8);append(bossRecords,UINT64_MAX,8);}
    m=decodeGameFrame(framed(bossRecords),true);check(m.structureComplete,"two Boss challenge records preserve complete fixed-width tuples");
    check(m.fields[6].meaningKnown && m.fields[6].value=="4294967295" && m.fields[6].name.find("BossChallengeGroup")!=std::string::npos,"Boss challenge group identity matches consumer lookup table");
    check(!m.fields[3].meaningKnown && !m.fields[7].meaningKnown && m.fields[7].value=="3735928559","Boss challenge fields ignored by consumer remain on wire");
    check(m.fields[9].value=="18446744073709551615" && m.fields[12].value=="4294967297" && m.fields.back().value=="18446744073709551615","Boss challenge opaque u64 and proven time maintain independent offsets");
    bossRecords.pop_back();check(!decodeGameFrame(framed(bossRecords),true).structureComplete,"Boss challenge last u64 truncation rejected");
    for(auto opcode:{0xe22eu,0xe226u,0xe237u}){Bytes empty;append(empty,opcode,2);empty.push_back(0);
        check(decodeGameFrame(framed(empty),true).structureComplete,"Daevanion and Boss lists accept zero entries");
        empty.pop_back();empty.insert(empty.end(),{0xff,0xff,0xff,0xff,0x0f});
        check(!decodeGameFrame(framed(empty),true).structureComplete,"Daevanion and Boss list count bounded before allocation");}
    Bytes presets=sample("8c e2 02 01 05 02");append(presets,UINT64_MAX,8);presets.push_back(0xff);
    append(presets,0x100000000ULL,8);presets.push_back(0x80);
    presets.insert(presets.end(),{3,0,4,4});auto presetFlagsAt=presets.size();
    presets.insert(presets.end(),{0,1,2,1,1,3});append(presets,0x80000000,4);
    presets.insert(presets.end(),{2,2,4,2});append(presets,0xffffffff,4);append(presets,0x7fffffff,4);
    presets.insert(presets.end(),{3,3,5});append(presets,99,4);presets.push_back(0);
    presets.insert(presets.end(),{6,1,7,8});append(presets,0xffffffff,4);
    presets.push_back(9);append(presets,0xdeadbeef,4);presets.insert(presets.end(),{1,10});append(presets,0xffffffff,4);
    presets.insert(presets.end(),{11,12,2,13,1});append(presets,0xfedcba98,4);presets.insert(presets.end(),{0xfe,14,0});
    m=decodeGameFrame(framed(presets),true);check(m.structureComplete,"preset sync covers six lists and all four skill optional combinations");
    size_t presetSelections=0;bool presetSigned=false,presetUnused=false,presetU64=false;
    for(const auto& f:m.fields){
        check(f.bit<0,"preset bytes must not be decoded as packed booleans");
        if(f.name.ends_with("当前预设键")){check(f.meaningKnown && f.type=="u8 LE","preset selections proven by per-system events");++presetSelections;}
        if(f.value=="-2147483648")presetSigned=f.type=="i32 LE" && !f.meaningKnown;
        if(f.value=="3735928559")presetUnused=!f.meaningKnown;
        if(f.value=="18446744073709551615")presetU64=!f.meaningKnown;
    }
    check(presetSelections==7 && presetSigned && presetUnused && presetU64,"preset sync preserves signed candidates, ignored scalar, and opaque u64 identities");
    for(size_t cut=2;cut<presets.size();++cut)check(!decodeGameFrame(framed(Bytes(presets.begin(),presets.begin()+cut)),true).structureComplete,"every preset field-boundary or mid-field truncation rejected");
    presets[presetFlagsAt]=4;check(!decodeGameFrame(framed(presets),true).structureComplete,"unknown skill preset optional flag rejected");
    check(decodeGameFrame(framed(sample("8c e2 01 00 02 00 03 00 04 00 05 00 00 00 00 00 06 07 00")),true).structureComplete,"empty preset tables still include seven selection bytes and unused u32");
    check(!decodeGameFrame(framed(sample("8c e2 01 ff ff ff ff 0f")),true).structureComplete,"preset count bounded before nested iteration");
    for(uint8_t flags=0;flags<4;++flags){
        Bytes pass{0x51,0xe3,flags};append(pass,0xffffffff,4);pass.push_back(7);
        append(pass,UINT64_MAX,8);append(pass,0,8);pass.push_back(1);
        if(flags&1){pass.push_back(2);append(pass,11,4);append(pass,12,4);}
        if(flags&2){pass.push_back(1);append(pass,13,4);}
        m=decodeGameFrame(framed(pass),true);
        check(m.structureComplete && m.fields[3].meaningKnown && m.fields[5].value=="18446744073709551615","membership pass optional combinations retain full timestamps and config identity");
        pass.pop_back();check(!decodeGameFrame(framed(pass),true).structureComplete,"membership pass truncated required or optional field rejected");
    }
    check(!decodeGameFrame(framed(sample("51 e3 04")),true).structureComplete,"membership pass unknown optional bits rejected");
    m=decodeGameFrame(framed(sample("af 8a 02 ff ff ff ff 07 01 00 00 00 07")),true);
    check(m.structureComplete && m.fields[3].name.find("AgitDecoItem")!=std::string::npos && m.fields[3].value=="4294967295" && !m.fields[4].meaningKnown,"decoration schema keeps configuration semantics distinct from shared wing reader");
    check(m.fields[4].value==m.fields[6].value,"duplicate decoration mapping keys retained on wire");
    check(decodeGameFrame(framed(sample("af 8a 00")),true).structureComplete,"empty decoration mapping supported");
    check(!decodeGameFrame(framed(sample("af 8a 01 01 00 00 00")),true).structureComplete,"decoration missing mapping byte rejected");
    // Eight booleans share the byte in C even across D and separate E groups.
    // The ninth boolean allocates a byte between E's second and third flags.
    Bytes multi=sample("5b 8d 01 78 56 34 12 34 12 cd ab 01 07 ff ff ff ff 03");
    append(multi,101,4);multi.push_back(0xa5);append(multi,102,4);append(multi,103,4);
    auto firstBoolOffset=size_t(1+2+1+8+1+5+1+4);
    multi.insert(multi.end(),{0x42,1,9,1,8,7});append(multi,UINT64_MAX,8);append(multi,0x87654321,4);
    multi.insert(multi.end(),{2,1,1,3,4,0xac,2,2,1,5,6,1,0x80,1});
    m=decodeGameFrame(framed(multi),true);check(m.structureComplete,"all five multi-state collections consume exact frame");
    size_t stateBits=0;const bool expectedBits[]={true,false,true,false,false,true,false,true,true};
    for(const auto& f:m.fields)if(f.bit>=0){
        check(f.value==(expectedBits[stateBits]?"true":"false") && f.bit==int(stateBits%8),"multi-state shared bit cursor crosses collection boundaries");
        if(stateBits<8)check(f.offset==firstBoolOffset,"cached bits refer to original C byte");
        ++stateBits;
    }
    check(stateBits==9 && m.fields.back().value=="128","ninth boolean and trailing multi-byte varint correctly located");
    bool retainedU64=false;for(const auto& f:m.fields)if(f.value=="18446744073709551615")retainedU64=!f.meaningKnown;
    check(retainedU64,"multi-state u64 preserved without assuming timestamp semantics");
    multi.pop_back();check(!decodeGameFrame(framed(multi),true).structureComplete,"multi-state trailing varint truncation rejected");
    check(decodeGameFrame(framed(sample("5b 8d 00 00 00 00 00 00")),true).structureComplete,"all multi-state collections may be empty");
    check(!decodeGameFrame(framed(sample("5b 8d ff ff ff ff 0f")),true).structureComplete,"multi-state hostile count rejected before allocation");
    auto mailFrame=framed(mail);m=decodeGameFrame(mailFrame,true);check(m.structureComplete,"nonempty mail entries include strings nested arrays and shared booleans");
    size_t mailBits=0,postIds=0,mailStrings=0;
    auto mailPrefix=splitGameFrames(mailFrame).frames[0].prefixBytes;
    for(const auto& f:m.fields){
        if(f.type=="bit"){check(f.offset==mailBitPositions[mailBits/8]+mailPrefix && f.bit==int(mailBits%8) && f.value==(mailBits%2?"false":"true"),"mail booleans cross entries and refill after eight bits");++mailBits;}
        if(f.name.find("Post 配置编号")!=std::string::npos){check(f.meaningKnown && f.value=="1234","mail template key follows Post config lookup");++postIds;}
        if(f.type=="UTF-8"){check(f.value=="中" && f.size==3,"mail text length counts UTF-8 bytes");++mailStrings;}
    }
    check(mailBits==10 && postIds==5 && mailStrings==5,"all five mail entries exercised");
    mail.pop_back();check(!decodeGameFrame(framed(mail),true).structureComplete,"mail trailing u16 truncation rejected");
    mail[13]=0xff;check(!decodeGameFrame(framed(mail),true).structureComplete,"mail oversized entry count rejected before iteration");
    Bytes select=sample("20 36 00 00 01 02 00 00 00 03 00 00 00 00 00 00 00 04 02 78 9c 01 05 00 00 00 06 00 00 00 07 01 08");append(select,UINT64_MAX,8);
    m=decodeGameFrame(framed(select),true);check(m.structureComplete && m.fields.back().value=="18446744073709551615","select-character response preserves both byte arrays nested tuple and final u64");
    select[18]=0x7f;check(!decodeGameFrame(framed(select),true).structureComplete,"select-character blob cannot exceed remaining bytes");
    const auto configFixture=sample("92 00 00 00 78 9c ab 66 50 62 70 64 c8 61 48 64 28 62 c8 65 f0 61 c8 67 28 67 f0 60 08 00 c2 54 a0 48 32 90 cc 63 28 01 aa b1 62 30 66 30 60 d0 01 b2 f2 80 62 c5 40 b1 54 86 14 b0 78 35 90 04 a9 4a 64 48 02 9a 03 13 2d 01 ea 2e 05 f2 6a c1 7a 72 18 32 c1 7a 40 32 d1 0c 86 40 31 23 86 58 b0 4c 31 58 4c 89 41 d7 cf f6 06 c3 3d 25 a0 7a 00 2d 65 19 0c");
    auto config=decodeCharacterConfig(configFixture);
    check(config.complete && config.expandedBytes==146 && config.topLevelKeys==4 && config.values.size()==5,"zlib UTF16 JSON expands nested objects and arrays");
    check(config.values[0].path=="/AlarmLowHPPercent" && config.values[0].value=="30" && config.values[1].path=="/nested/enabled" && config.values[1].value=="true","JSON source names and typed values preserved");
    check(config.values.back().value=="\"中😀\"","UTF16 surrogate pair converted to valid UTF8");
    auto badConfig=configFixture;badConfig.back()^=1;check(!decodeCharacterConfig(badConfig).complete,"zlib checksum mismatch rejected");
    badConfig=configFixture;badConfig.push_back(0);check(!decodeCharacterConfig(badConfig).complete,"trailing bytes after zlib stream rejected");
    badConfig=configFixture;badConfig[0]-=2;check(!decodeCharacterConfig(badConfig).complete,"zlib output length smaller than actual rejected");
    badConfig=configFixture;badConfig[0]+=2;check(!decodeCharacterConfig(badConfig).complete,"zlib output length larger than actual rejected");
    badConfig=configFixture;badConfig[3]=1;check(!decodeCharacterConfig(badConfig).complete,"zlib allocation cap enforced before decompression");
    check(!decodeCharacterConfig(sample("12 00 00 00 78 9c ab 66 50 62 a8 00 62 2b 20 66 b8 a1 c4 50 cb 00 00 1c 0e 03 0b")).complete,"unpaired UTF16 surrogate rejected without replacement");
    check(!decodeCharacterConfig(sample("b4 00 00 00 78 9c ab 66 50 62 48 61 48 05 c2 02 20 cb 8a 21 9a ca d0 80 21 96 ca b0 96 01 00 35 f1 20 05")).complete,"deep JSON nesting rejected before traversal");
    auto compressed=Bytes{0x16,0xff,0xff,11,0,0,0,0xb0};compressed.insert(compressed.end(),time.begin(),time.end());
    m=decodeGameFrame(compressed,true);check(m.structureComplete && m.expanded==time,"LZ4 literal envelope");
    check(decompressLz4Block(sample("13 41 01 00 50 42 43 44 45 46"),13)==sample("41 41 41 41 41 41 41 41 42 43 44 45 46"),"LZ4 overlapping match");
    bool bad=false;try{decompressLz4Block(sample("10 41 00 00 00"),8);}catch(...){bad=true;}check(bad,"LZ4 zero offset rejected");
    bad=false;try{decompressLz4Block(sample("f0 ff"),100);}catch(...){bad=true;}check(bad,"LZ4 truncated length rejected");
    bad=false;try{decompressLz4Block(Bytes{0},9000000);}catch(...){bad=true;}check(bad,"LZ4 output cap");
    CipherSnapshot identity;for(size_t i=0;i<256;++i)identity.table[i]=uint8_t(i);
    Bytes zeros(5);identity.transform(zeros);check(zeros==sample("00 02 05 07 0d"),"observed PRGA starts at stored index before increment");
    m=decodeGameFrame(sample("1a 02 38 d3 3e 00 d9 8e 15 01 85 00 d3 3e f9 f4 ab 43 b7 82 01 01 00"),true);
    check(m.structureComplete && m.fields[2].value=="8019","captured 3802 frame");
    check(m.fields[8].type=="float32 LE" && m.fields[8].value.starts_with("343.913") && !m.fields[8].meaningKnown,"skill-target yaw uses reflected float without claiming unverified message mapping");
    auto skillStartAll=sample("02 38 01 3f 28 46 0f 00 14 15 03 02 07 00 00 00 00 00 34 43 00 00 c0 3f 00 00 10 c0 00 00 60 40 64 02 09 0a 03 0b 01 01 04 00 00 80 bf 00 00 00 40 00 00 40 40");
    auto startAll=decodeGameFrame(framed(skillStartAll),true);
    check(startAll.structureComplete && startAll.fields.back().value=="3" && startAll.fields.back().type=="float32 LE","skill-start all optional fields including projectile float coordinates");
    bool targetNegative=false,flight=false,proxy=false;
    for(const auto& field:startAll.fields){targetNegative|=field.name=="目标位置 Y（反射对应）" && field.value=="-2.25";flight|=field.name=="动作移动类型（反射对应）" && field.value=="2 / Flight";proxy|=field.name=="代理施放技能（反射对应）" && field.value=="true";}
    check(targetNegative && flight && proxy,"nested vector, reflected movement enum and packed proxy bit retain independent meanings");
    for(size_t n=2;n<skillStartAll.size();++n)check(!decodeGameFrame(framed(Bytes(skillStartAll.begin(),skillStartAll.begin()+n)),true).structureComplete,"skill-start optional nested fields reject truncation");
    auto compound=sample("1f 04 38 d3 3e 00 00 d3 3e d9 8e 15 01 85 02 cf cc 6b 6c 01 00 00 00 b7 82 01 01 00");
    m=decodeGameFrame(compound,true);check(m.structureComplete,"captured 3804 frame");
    m=decodeGameFrame(sample("32 2b 38 d3 3e 13 37 7b 94 d7 0a 60 09 00 00 00 00 00 00 43 48 47 eb a0 01 00 00 d3 3e 15 d9 8e 15 01 02 8d 9e a3 c7 37 04 e8 c7 00 5d 04 47"),true);
    check(m.structureComplete && m.fields.back().bit==1 && m.fields.back().value=="true","captured 382B bool after vector reuses earlier byte");
    compound[5]=4;compound[0]+=10;auto maximum=sample("ff ff ff ff ff ff ff ff ff 01");compound.insert(compound.end()-2,maximum.begin(),maximum.end());
    m=decodeGameFrame(compound,true);check(m.structureComplete && m.fields[m.fields.size()-2].value=="18446744073709551615","full unsigned 64-bit varint");
    compound[compound.size()-3]=2;m=decodeGameFrame(compound,true);check(!m.structureComplete && m.status=="无效的 64 位变长整数","u64 varint overflow rejected");
    compound=sample("24 04 38 d3 3e 20 00 d3 3e d9 8e 15 01 85 02 cf cc 6b 6c 01 00 00 00 b7 82 01 ff ff ff ff 07 01 00");
    m=decodeGameFrame(compound,true);check(!m.structureComplete && m.status=="数组数量超过剩余字节或分析限制","hostile array count bounded before iteration");
    if(argc>1) {
        const auto dir=std::filesystem::path(argv[1]);auto state=loadCipherSnapshot(dir/"cipher-session.a2cs");
        auto read=[](const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Missing evidence file");return Bytes(std::istreambuf_iterator<char>(f),{});};
        if(argc>4){
            auto queryResponse=read(argv[4]);auto query=decodeGameFrame(queryResponse,true);
            auto field=[&](const char* name){for(const auto& f:query.fields)if(f.name==name)return f.value;return std::string{};};
            check(queryResponse.size()==4674 && !query.structureComplete && query.parsedBytes==4122 && query.skinsComplete && query.skins.size()==39,"captured query response decodes verified equipment while preserving the remaining tail");
            check(field("资料查询结果（0=成功；其他值待确认）")=="0" && field("资料角色等级")=="50" && field("资料角色在线")=="true","captured normal query succeeded and returned level and online state");
        }
        auto plain=read(dir/"cipher-plain.bin"),cipher=read(dir/"cipher-wire.bin");auto transformed=plain;auto copy=state;copy.transform(transformed);
        check(transformed==cipher,"x64dbg plain to actual wire transform");
        check(cipher.size()<124,"small anchor evidence fixture");
        Bytes frame{uint8_t(cipher.size()+4)};frame.insert(frame.end(),cipher.begin(),cipher.end());
        auto decoded=decryptGameStream(frame,state.frameSequence,state.source,state.destination,state);
        check(Bytes(decoded.begin()+1,decoded.end())==plain,"sequence-anchored decryption");
        check(decodeGameFrame(decoded,true,true).structureComplete,"outbound time echo schema");
        bad=false;try{decryptGameStream(frame,state.frameSequence+1,state.source,state.destination,state);}catch(...){bad=true;}check(bad,"refuse missing cipher anchor");
        bad=false;try{decryptGameStream(frame,state.frameSequence,state.destination,state.source,state);}catch(...){bad=true;}check(bad,"refuse wrong cipher flow");
    }
    std::mt19937 rng(37);
    if(argc>2) {
        auto packets=loadPcap(argv[2]);std::map<std::string,Stream> streams;
        for(const auto& p:packets)if(p.protocol==6 && p.source.port==13328)streams[p.directionKey()].add(p);
        size_t outer=0,inner=0;
        for(const auto& [key,stream]:streams){check(!stream.hasGap(),"regression capture has no TCP gaps");auto frames=splitGameFrames(stream.bytes);check(frames.consumed==stream.bytes.size(),"regression stream fully framed");
            for(const auto& f:frames.frames){auto decoded=decodeGameFrame(std::span(stream.bytes).subspan(f.offset,f.length),true);check(decoded.structureComplete,"regression outer message fully parsed");++outer;
                if(!decoded.expanded.empty()){auto children=splitGameFrames(decoded.expanded);check(children.consumed==decoded.expanded.size(),"regression decompressed stream fully framed");for(auto& child:children.frames){check(decodeGameFrame(std::span(decoded.expanded).subspan(child.offset,child.length),true).structureComplete,"regression nested message fully parsed");++inner;}}
            }
        }
        check(outer==3868 && inner==25,"recorded movement session coverage");
    }
    if(argc>3) {
        auto packets=loadPcap(argv[3]);std::map<std::string,Stream> streams;std::map<uint16_t,size_t> observed;
        std::vector<std::string> characterKeys;std::string selectedCharacterKey;std::vector<std::string> preparePosition,completePosition;
        for(const auto& p:packets)if(p.protocol==6 && p.source.port==13328)streams[p.directionKey()].add(p);
        std::function<void(std::span<const uint8_t>,size_t)> inspect=[&](auto bytes,size_t depth){
            check(depth<4,"login regression nesting bounded");auto frames=splitGameFrames(bytes);
            check(frames.consumed==bytes.size(),"login regression message boundaries exact");
            for(const auto& frame:frames.frames){auto wire=bytes.subspan(frame.offset,frame.length);auto decoded=decodeGameFrame(wire,true);
                auto opcode=uint16_t(readInteger(wire,frame.prefixBytes,2,false));
                if(opcode==0x3900){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size() && decoded.fields[2].value=="16","captured specialized skill list consumes sixteen parent groups");++observed[opcode];
                    size_t slotCount=0;for(const auto& field:decoded.fields)if(field.name.ends_with(" 类型"))++slotCount;
                    check(slotCount==56,"all fifty-six captured specialized slots decoded");
                    std::ofstream proof(std::filesystem::path(argv[1])/"specialized-skill-captured-frame.bin",std::ios::binary);proof.write(reinterpret_cast<const char*>(wire.data()),std::streamsize(wire.size()));
                }
                if(opcode==0x5100){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size() && decoded.fields[2].value=="70","captured skill list has seventy entries and exact boundaries");++observed[opcode];
                    std::ofstream proof(std::filesystem::path(argv[1])/"skill-list-captured-frame.bin",std::ios::binary);proof.write(reinterpret_cast<const char*>(wire.data()),std::streamsize(wire.size()));
                }
                if(opcode==0x382A || opcode==0x3813 || opcode==0x380F || opcode==0x364A || opcode==0x8D00 || opcode==0x3656 || opcode==0x8A33 || opcode==0x382C || opcode==0x3611 || opcode==0x3615 || opcode==0x5684 || opcode==0xFFA9 || opcode==0xE215 || opcode==0xE263 || opcode==0x3620){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"new login structure consumes exact frame");++observed[opcode];
                }
                if(opcode==0x3641 || opcode==0x3645){
                    check(decoded.structureComplete,"entity initialization remains completely parsed");size_t hpFields=0;
                    for(const auto& field:decoded.fields){
                        if(field.name=="状态结构 HP" || field.name=="状态结构 HP 上限"){
                            check(field.meaningKnown && field.type=="ULEB128 / u64","entity HP retains unsigned variable-width representation");++hpFields;
                        }
                        if(field.name.starts_with("属性条目 ") && field.type=="i32 LE"){
                            Bytes mutated(wire.begin(),wire.end());for(size_t k=0;k<4;++k)mutated[field.offset+k]=0xff;
                            auto negative=decodeGameFrame(mutated,true);bool found=false;
                            for(const auto& nf:negative.fields)if(nf.offset==field.offset && nf.type=="i32 LE")found=nf.value=="-1";
                            check(negative.structureComplete && found,"entity stat value follows client signed extension without losing boundaries");break;
                        }
                    }
                    check(hpFields==2,"both initial HP fields identified");++observed[opcode];
                }
                if(opcode==0x3620){
                    for(const auto& field:decoded.fields)if(field.name=="选中角色的列表编号"){
                        check(field.meaningKnown && field.type=="ULEB128" && field.offset==6 && field.size==2,"selected character key uses proven varint field");selectedCharacterKey=field.value;
                    }
                    check(decoded.config.complete && decoded.config.expandedBytes==7796 && decoded.config.topLevelKeys==52,"recorded character config zlib UTF16 JSON fully verified");
                    bool alarm=false;for(const auto& v:decoded.config.values)if(v.path=="/AlarmLowHPPercent")alarm=v.type=="number";
                    check(alarm && !decoded.config.values.empty(),"recorded low-HP setting identified by source JSON key");
                }
                if(opcode==0xE25A){
                    check(decoded.structureComplete && decoded.parsedBytes==220,"captured SDK response exactly consumed");
                    check(decoded.fields.back().offset==8 && decoded.fields.back().size==212 && !decoded.fields.back().meaningKnown,"captured SDK blob has verified boundaries and unresolved semantics");++observed[opcode];
                }
                if(opcode==0x8D44 || opcode==0x8D53 || opcode==0x8D58){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured settings and classified data consume exact frame");++observed[opcode];
                }
                if(opcode==0x5648 || opcode==0x564B){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured auto-use message exactly consumed");++observed[opcode];
                    if(opcode==0x5648)check(decoded.fields[2].value=="1" && decoded.fields[3].value=="1" && decoded.fields[4].meaningKnown && decoded.fields[5].value=="0","recorded primary potion and empty fallback slot identified");
                    else check(decoded.fields[2].value=="6" && decoded.fields.size()==9,"six recorded auto-use Item IDs decoded");
                }
                if(opcode==0x5672 || opcode==0x5679 || opcode==0x5682 || opcode==0x5683){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured wing title and achievement frames exactly consumed");++observed[opcode];
                }
                if(opcode==0x561D || opcode==0x5633 || opcode==0x56AC || opcode==0x56B6){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured equipment godstone categories and account records exactly consumed");++observed[opcode];
                    if(opcode==0x5633)check(decoded.fields[2].value=="2" && decoded.fields.back().value=="1","two saved godstones decoded");
                    if(opcode==0x56AC)check(decoded.fields[2].value=="59" && decoded.fields.size()==62,"all 59 captured category bytes retained");
                    if(opcode==0x56B6)check(decoded.fields[2].value=="0","actual account sample covers only empty list");
                    if(opcode==0x561D)check(decoded.fields[2].value=="3099" && decoded.fields[3].value=="3099","recorded equipment-level values retained without inferred scale");
                }
                if(opcode==0x3746){
                    check(decoded.structureComplete && decoded.fields.size()==4 && decoded.fields[2].value=="3558" && decoded.fields[3].value=="33462","captured ground reference height agrees with native reader");++observed[opcode];
                }
                if(opcode==0x3646 || opcode==0x3649){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured barrier and all-stat messages consume exact frames");++observed[opcode];
                    if(opcode==0x3646)check(decoded.fields[2].value=="3558" && decoded.fields[3].value=="0","captured barrier covers empty list only");
                    else check(decoded.fields[2].value=="0" && decoded.fields[3].value=="151","captured all-stat response has 151 entries");
                }
                if(opcode==0x3633){
                    check(decoded.structureComplete && decoded.parsedBytes==2962,"captured self appearance exact byte coverage");++observed[opcode];
                    for(const auto& field:decoded.fields){if(field.name=="等级")check(field.value=="45","captured self level");if(field.name=="实体编号")check(field.value=="3558","captured self key matches character selection");}
                }
                if(opcode==0x3621 || opcode==0x3623 || opcode==0x8D4D){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"recorded map and item response consumes exact frame");++observed[opcode];
                    if(opcode==0x3621 || opcode==0x3623){auto& position=opcode==0x3621?preparePosition:completePosition;
                        for(const auto& field:decoded.fields)if(field.name.ends_with(" X") || field.name.ends_with(" Y") || field.name.ends_with(" Z")){
                            check(field.meaningKnown && field.type=="float32 LE","map coordinate type and semantic evidence retained");position.push_back(field.value);}
                    }
                }
                if(opcode==0x9507 || opcode==0x9508 || opcode==0x56A8 || opcode==0xE274){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured social action customizing and community messages consume exact frame");++observed[opcode];
                }
                if(opcode==0x572D || opcode==0x570F || opcode==0x8A45 || opcode==0xE349 || opcode==0xE34E){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"remaining recorded outer response exactly consumed");++observed[opcode];
                    if(opcode==0x572D)check(decoded.fields[2].value=="0","recorded special distribution covers empty list only");
                    if(opcode==0x570F)check(decoded.fields[3].value=="0","recorded MonsterCube reward response covers empty list only");
                    if(opcode==0xE349)check(decoded.fields[4].value=="1" && decoded.fields[9].value=="AION2","recorded additional mail includes one nonempty entry");
                }
                if(opcode==0xE22E || opcode==0xE226 || opcode==0xE237){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured Daevanion and Boss challenge messages consume exact frame");++observed[opcode];
                    if(opcode==0xE22E)check(decoded.fields[2].value=="5" && decoded.fields[3].value=="1 / DaevanionCrystal" && decoded.fields[4].value=="519","five recorded Daevanion point types decoded");
                    if(opcode==0xE226){size_t nodeKeys=0;for(const auto& f:decoded.fields)if(f.name.find("DaevanionNode 配置编号")!=std::string::npos){check(f.meaningKnown,"captured node identity has table evidence");++nodeKeys;}
                        check(decoded.fields[2].value=="8" && nodeKeys==422,"eight recorded Daevanion groups contain 422 nodes");}
                    if(opcode==0xE237)check(decoded.fields[2].value=="11" && decoded.fields[6].value=="1001","eleven recorded Boss challenge groups decoded");
                }
                if(opcode==0xE28C){
                    check(decoded.structureComplete && decoded.parsedBytes==1252,"recorded multi-system preset frame exactly consumed");++observed[opcode];
                    size_t selections=0,candidateLists=0;
                    for(const auto& f:decoded.fields){
                        if(f.name.ends_with("当前预设键")){check(f.value=="1" && f.meaningKnown,"all recorded current preset keys are one");++selections;}
                        if(f.name.ends_with("候选数量")){check(f.value=="4","each recorded skill preset carries four candidates");++candidateLists;}
                    }
                    check(selections==7 && candidateLists==36,"all recorded system selections and skill candidate lists decoded");
                }
                if(opcode==0x610B || opcode==0x6103 || opcode==0x9232 || opcode==0x9330){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"recorded ticket dungeon teleport mapping frame exactly consumed");++observed[opcode];
                    const auto expected=opcode==0x610B?"76":opcode==0x6103?"2":opcode==0x9232?"74":"0";
                    check(decoded.fields[2].value==expected,"recorded list count agrees with all consumed entries");
                    if(opcode==0x9232)for(size_t i=3;i<decoded.fields.size();++i)check(decoded.fields[i].meaningKnown,"every captured teleport configuration key is identified");
                }
                if(opcode==0xE200 || opcode==0xE324){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured card initialization and pass group list consume exact frames");++observed[opcode];
                    if(opcode==0xE200)check(wire.size()==15 && decoded.fields.size()==7 && decoded.fields[2].value=="0" && decoded.fields[3].value=="18446744073680751616" && decoded.fields[6].value=="0","empty captured card and block lists preserve reset-time raw bits and mandatory counters");
                    if(opcode==0xE324){check(wire.size()==437 && decoded.fields.size()==132 && decoded.fields[2].value=="9" && decoded.fields[3].value=="20041" && decoded.fields[6].value=="1790148157203" && decoded.fields[7].value=="18446741864691951616","nine captured pass groups and raw time sentinel preserved");
                        size_t flags=0;for(const auto& field:decoded.fields)if(field.type=="bit")++flags;check(flags==18,"all 18 captured pass flags parsed across group boundaries");}
                }
                if(opcode==0xE223 || opcode==0xE25C || opcode==0xE262 || opcode==0xE30B){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"recorded trust admin Monolith and fixed notification consume exact frames");++observed[opcode];
                    if(opcode==0xE223)check(wire.size()==15 && !decoded.fields[2].meaningKnown && !decoded.fields[3].meaningKnown,"captured fixed notification values remain unknown");
                    if(opcode==0xE25C)check(wire.size()==6 && decoded.fields[3].value=="75 / ItemRestore" && decoded.fields[4].value=="true","captured administrator item restore flag confirmed");
                    if(opcode==0xE30B)check(wire.size()==28 && decoded.fields[2].value=="3" && decoded.fields[3].value=="2001" && decoded.fields[4].value=="560" && decoded.fields[8].value=="300","captured three Monolith resonance records confirmed");
                    if(opcode==0xE262)check(wire.size()==121 && decoded.fields[3].value=="4" && decoded.fields[2].value=="false" && decoded.fields[24].value=="true" && decoded.fields[24].offset==decoded.fields[2].offset,"captured four trust quotas and shared flags confirmed");
                }
                if(opcode==0xE224 || opcode==0xE256 || opcode==0xE257){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured contents control unlock and period collection consume exact frame");++observed[opcode];
                    if(opcode==0xE224)check(wire.size()==7 && decoded.fields.size()==7 && decoded.fields[3].value=="1 / Exchange" && decoded.fields[5].value=="2 / DreamCrossRoad" && decoded.fields[4].value=="true" && decoded.fields[6].value=="true" && decoded.fields[4].offset==decoded.fields[6].offset,"captured control map shares one bool byte");
                    if(opcode==0xE257)check(wire.size()==62 && decoded.fields.size()==61 && decoded.fields[2].value=="58","58 captured unlock enum entries preserved");
                    if(opcode==0xE256)check(wire.size()==2102 && decoded.fields[2].value=="6" && decoded.fields[3].value=="6405417" && decoded.fields[4].value=="1 / Tab_01","six captured period collections begin at confirmed collection and tab");
                }
                if(opcode==0xE32A || opcode==0xE33B || opcode==0xE33D || opcode==0xE33E || opcode==0xE342 || opcode==0xE343){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured ranking subscription exchange seasonal and restore messages exactly consumed");++observed[opcode];
                    if(opcode==0xE32A)check(wire.size()==46 && decoded.fields.size()==24 && decoded.fields[2].value=="3" && decoded.fields[3].value=="1 / Single" && decoded.fields[4].value=="1500","captured three arena rankings have confirmed points");
                    if(opcode==0xE33B || opcode==0xE33E || opcode==0xE342)check(wire.size()==4 && decoded.fields.size()==3 && decoded.fields[2].value=="0","captured empty lists do not invent nonempty sample data");
                    if(opcode==0xE33D)check(wire.size()==7 && decoded.fields[2].value=="0","captured personal exchange count is zero");
                    if(opcode==0xE343)check(wire.size()==67 && decoded.fields.size()==15 && decoded.fields[2].value=="3" && decoded.fields[4].value=="1 / NpcShopSell" && decoded.fields[8].value=="2 / Extraction" && decoded.fields[12].value=="3 / AttuneSoulBind","captured restore statuses use all three operational enum values");
                }
                if(opcode==0x8D8C || opcode==0x8D09 || opcode==0x5712){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured quest ceiling and second pair lists exactly consumed");++observed[opcode];
                    if(opcode==0x8D8C)check(wire.size()==3038 && decoded.fields.size()==761 && decoded.fields[2].value=="379" && decoded.fields[3].value=="12013001","379 captured second-list pairs preserved");
                    if(opcode==0x8D09)check(wire.size()==4828 && decoded.fields.size()==2850 && decoded.fields[2].value=="219" && decoded.fields[3].value=="2602000" && decoded.fields[4].value=="4 / Rewarded" && decoded.fields[12].value=="8","219 captured quests and first rewarded quest UID verified");
                    if(opcode==0x5712)check(wire.size()==76 && decoded.fields.size()==39 && decoded.fields[2].value=="12" && decoded.fields[3].value=="600001" && decoded.fields[4].value=="0" && decoded.fields[5].value=="3","12 captured ceiling records use distinct Dungeon keys and bytes");
                }
                if(opcode==0x8D7E || opcode==0x8D1C || opcode==0x8D37){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured pair gathering and nested duty messages exactly consumed");++observed[opcode];
                    if(opcode==0x8D7E)check(wire.size()==3974 && decoded.fields.size()==995 && decoded.fields[2].value=="496" && decoded.fields[3].value=="121012003","all 496 captured pairs decoded");
                    if(opcode==0x8D1C)check(wire.size()==1413 && decoded.fields.size()==398 && decoded.fields[4].value=="15" && decoded.fields[11].value=="930100023" && decoded.fields[395].value=="1788296400000" && decoded.fields[397].value=="1788296400000","captured nested duty groups items and both timestamps align");
                    if(opcode==0x8D37)check(wire.size()==80 && decoded.fields.size()==23 && decoded.fields[2].value=="9" && decoded.fields[3].value=="100" && decoded.fields[4].value=="9" && decoded.fields[21].value=="0" && decoded.fields[22].value=="0","nine captured gathering skills include both trailing points");
                }
                if(opcode==0x9331 || opcode==0x3657){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured season and mentoring messages exactly consumed");++observed[opcode];
                    if(opcode==0x9331)check(decoded.fields[2].value=="2" && decoded.fields[4].value=="2" && decoded.fields[15].value=="2" && decoded.fields.size()==25,"both captured season records use confirmed group key 2");
                    else check(decoded.fields[2].value=="3 / Mentee_Inactive" && decoded.fields[3].value=="0","captured mentoring semantics agree with enum metadata");
                }
                if(opcode==0xE351 || opcode==0x8AAF || opcode==0x8D5B){
                    check(decoded.structureComplete && decoded.parsedBytes==wire.size(),"captured pass decoration and multi-state messages exactly consumed");++observed[opcode];
                    if(opcode==0xE351)check(decoded.fields[2].value=="0" && decoded.fields[3].value=="1","actual pass sample has no optional lists");
                    if(opcode==0x8AAF)check(decoded.fields[2].value=="22","all captured decoration mappings decoded");
                    if(opcode==0x8D5B){size_t bits=0;for(const auto& f:decoded.fields)if(f.bit>=0)++bits;
                        check(decoded.fields[2].value=="404" && bits==52,"captured multi-state has 404 A entries and 52 shared bits");}
                }
                if(opcode==0x3615){
                    size_t names=0,pcData=0;
                    for(const auto& field:decoded.fields){
                        if(field.name.ends_with(" 角色列表编号")){check(field.meaningKnown && field.type=="u32 LE","login character key retains fixed-width encoding");characterKeys.push_back(field.value);}
                        if(field.name.ends_with(" 角色名")){check(field.meaningKnown && field.type=="UTF-8" && !field.value.empty(),"recorded character name is readable UTF8");++names;}
                        if(field.name.ends_with(" PcData 配置编号")){check(field.meaningKnown && field.type=="u32 LE","PcData key has proven table evidence");++pcData;}
                    }
                    check(names==characterKeys.size() && pcData==characterKeys.size() && names>0,"each character has a key name and PcData key");
                }
                if(!decoded.expanded.empty())inspect(decoded.expanded,depth+1);
            }
        };
        for(const auto& [key,stream]:streams){check(!stream.hasGap(),"login regression no TCP gaps");inspect(stream.bytes,0);}
        check(selectedCharacterKey=="3558" && std::find(characterKeys.begin(),characterKeys.end(),selectedCharacterKey)!=characterKeys.end(),"selected character key matches a recorded login character");
        check(observed[0x382A]==52 && observed[0x3813]==22 && observed[0x380F]==16,"90 recorded login structures covered");
        check(observed[0x364A]==9 && observed[0x8D00]==7,"16 recorded stat and resource structures covered");
        check(observed[0x3656]==3,"all three captured combat-power updates consumed");
        check(observed[0x8A33]==4 && observed[0x382C]==3,"four guild updates and three abnormal removals consume exact frames");
        check(observed[0x3611]==1 && observed[0x3615]==1,"world handshake and nested login response fully parsed");
        check(observed[0x5684]==2 && observed[0xFFA9]==2,"four captured status maps and abyss limits consume exact frame");
        check(observed[0xE215]==2 && observed[0xE263]==1 && observed[0x3620]==1,"captured mail responses trade state and character selection consume exact frame");
        check(observed[0xE25A]==1,"one captured SDK response envelope covered");
        check(observed[0x8D44]==1 && observed[0x8D53]==1 && observed[0x8D58]==1,"three captured settings and classified data responses covered");
        check(observed[0x5648]==1 && observed[0x564B]==1,"both recorded auto-use lists covered");
        check(observed[0x5672]==1 && observed[0x5679]==1 && observed[0x5682]==1 && observed[0x5683]==1,"all four recorded wing title achievement initial messages covered");
        check(observed[0x561D]==1 && observed[0x5633]==1 && observed[0x56AC]==1 && observed[0x56B6]==1,"all four captured equipment godstone category account messages covered");
        check(observed[0x3621]==1 && observed[0x3623]==1 && observed[0x8D4D]==1,"all three map and item responses covered");
        check(observed[0x9507]==1 && observed[0x9508]==1 && observed[0x56A8]==1 && observed[0xE274]==1,"all four captured social and customizing responses covered");
        check(observed[0x572D]==1 && observed[0x570F]==1 && observed[0x8A45]==1 && observed[0xE349]==1 && observed[0xE34E]==1,"all five remaining recorded outer schemas covered");
        check(observed[0xE22E]==1 && observed[0xE226]==1 && observed[0xE237]==1,"all three recorded Daevanion and Boss challenge schemas covered");
        check(observed[0xE28C]==1,"captured multi-system preset synchronization covered");
        check(observed[0x610B]==1 && observed[0x6103]==1 && observed[0x9232]==1 && observed[0x9330]==1,"all four recorded ticket dungeon teleport and mapping lists covered");
        check(observed[0x9331]==1 && observed[0x3657]==1,"both recorded season and mentoring messages covered");
        check(observed[0x8D7E]==1 && observed[0x8D1C]==1 && observed[0x8D37]==1,"all three pair gathering and duty capture fixtures covered");
        check(observed[0x8D8C]==1 && observed[0x8D09]==1 && observed[0x5712]==1,"all new quest ceiling and second pair fixtures covered");
        for(auto opcode:{0xE32A,0xE33B,0xE33D,0xE33E,0xE342,0xE343})check(observed[uint16_t(opcode)]==1,"all six new E3 capture fixtures covered");
        for(auto opcode:{0xE224,0xE256,0xE257})check(observed[uint16_t(opcode)]==1,"all new contents and collection capture fixtures covered");
        for(auto opcode:{0xE223,0xE25C,0xE262,0xE30B})check(observed[uint16_t(opcode)]==1,"all trust admin Monolith and fixed notification capture fixtures covered");
        for(auto opcode:{0xE200,0xE324})check(observed[uint16_t(opcode)]==1,"both captured card and pass group messages covered");
        check(observed[0xE351]==1 && observed[0x8AAF]==1 && observed[0x8D5B]==1,"all three captured pass decoration multi-state schemas covered");
        check(preparePosition.size()==3 && preparePosition==completePosition,"recorded prepared position equals completed-map position on all three axes");
        check(observed[0x3641]>0 && observed[0x3645]>0 && observed[0x3633]==1,"self and surrounding entity initialization schemas exercised by capture");
        check(observed[0x3646]==1 && observed[0x3649]==1,"both captured barrier and all-stat responses covered");
        check(observed[0x3746]==1,"captured ground reference message covered");
        streams.clear();size_t loginMessages=0;
        for(const auto& p:packets)if(p.protocol==6 && p.source.port==13700)streams[p.directionKey()].add(p);
        for(const auto& [key,stream]:streams){check(!stream.hasGap(),"login-service stream without TCP gap");auto frames=splitGameFrames(stream.bytes);
            check(frames.consumed==stream.bytes.size(),"login-service framing consumes stream");
            for(const auto& frame:frames.frames){auto decoded=decodeGameFrame(std::span(stream.bytes).subspan(frame.offset,frame.length),true,false,GameProfile::Login);
                check(decoded.structureComplete && decoded.parsedBytes==frame.length,"login-service frame exactly consumed");++loginMessages;}
        }
        check(loginMessages==12,"all recorded login-service inbound frames covered");
    }
    for(int n=0;n<10000;++n){Bytes b(rng()%256);for(auto& c:b)c=uint8_t(rng());splitGameFrames(b);decodeGameFrame(b,true);decodeGameFrame(b,true,false,GameProfile::Login);}
    std::cout<<checks<<" game protocol checks and 10000 malformed cases per profile passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
