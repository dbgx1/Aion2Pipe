#include "game_protocol.hpp"
#include "stat_names.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
namespace aion {
namespace {
struct Var { uint32_t value{}; size_t size{}; bool complete{}, invalid{}; };
Var varint(std::span<const uint8_t> b,size_t maxBytes) {
    Var v;
    for(size_t i=0;i<b.size() && i<maxBytes;++i) {
        const auto c=b[i];
        if(i==4 && (c&0xF0)) {v.invalid=true;return v;}
        v.value|=uint32_t(c&127)<<(7*i); ++v.size;
        if(!(c&128)) {v.complete=true;return v;}
    }
    v.invalid=v.size==maxBytes; return v;
}
std::string number(double v) {std::ostringstream s;s<<std::setprecision(9)<<v;return s.str();}
std::optional<std::string> utf8Preview(std::span<const uint8_t> bytes) {
    std::string text;bool clipped=false;
    for(size_t pos=0;pos<bytes.size();){auto start=pos;uint32_t code=bytes[pos++];unsigned extra=0;uint32_t minimum=0;
        if(code>=0xc2 && code<=0xdf){code&=31;extra=1;minimum=0x80;}
        else if(code>=0xe0 && code<=0xef){code&=15;extra=2;minimum=0x800;}
        else if(code>=0xf0 && code<=0xf4){code&=7;extra=3;minimum=0x10000;}
        else if(code>=0x80)return {};
        if(extra>bytes.size()-pos)return {};
        for(unsigned i=0;i<extra;++i){auto c=bytes[pos++];if((c&0xc0)!=0x80)return {};code=(code<<6)|(c&63);}
        if(code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff))return {};
        if(start>=512){clipped=true;continue;}
        if(code<32 || code==127){const char* h="0123456789ABCDEF";text+="\\x";text+=h[code>>4];text+=h[code&15];}
        else if(code=='\\')text+="\\\\";
        else text.append(reinterpret_cast<const char*>(bytes.data()+start),pos-start);
    }
    if(clipped)text+=" …";return text;
}
constexpr size_t maxAnalysisFields=32768;
struct Reader {
    std::span<const uint8_t> b; GameMessage& out; size_t pos{},boolPos{},boolBit=8;
    void need(size_t n) {if(pos>b.size() || n>b.size()-pos)throw std::runtime_error("字段截断");}
    void add(size_t at,size_t n,const std::string& name,const std::string& type,const std::string& value,const std::string& evidence,bool known,int bit=-1) {
        if(out.fields.size()>=maxAnalysisFields)throw std::runtime_error("字段数量超过限制");
        out.fields.push_back({at,n,name,type,value,evidence,known,bit});
    }
    uint64_t u(const std::string& name,size_t n,const std::string& evidence,bool known=true) {
        need(n);auto at=pos; auto v=readInteger(b,pos,n,false);pos+=n;
        add(at,n,name,"u"+std::to_string(n*8)+" LE",std::to_string(v),evidence,known);return v;
    }
    void i8(const std::string& name,const std::string& evidence) {
        need(1);auto at=pos++;int v=b[at];if(v>=128)v-=256;
        add(at,1,name,"i8",std::to_string(v),evidence,true);
    }
    void i16(const std::string& name,const std::string& evidence,bool known) {
        need(2);auto at=pos;auto v=std::bit_cast<int16_t>(uint16_t(readInteger(b,pos,2,false)));pos+=2;
        add(at,2,name,"i16 LE",std::to_string(v),evidence,known);
    }
    void i32(const std::string& name,const std::string& evidence,bool known) {
        need(4);auto at=pos;auto v=std::bit_cast<int32_t>(uint32_t(readInteger(b,pos,4,false)));pos+=4;
        add(at,4,name,"i32 LE",std::to_string(v),evidence,known);
    }
    uint32_t var(const std::string& name,const std::string& evidence,bool known=true) {
        auto v=varint(b.subspan(pos),5);if(!v.complete)throw std::runtime_error(v.invalid?"无效的 32 位变长整数":"变长整数截断");
        auto at=pos;pos+=v.size;add(at,v.size,name,"ULEB128",std::to_string(v.value),evidence,known);return v.value;
    }
    uint64_t var64(const std::string& name,const std::string& evidence,bool known=false) {
        uint64_t value=0;const auto at=pos;
        for(size_t i=0;i<10;++i){need(1);auto c=b[pos++];if(i==9 && (c&0xfe))throw std::runtime_error("无效的 64 位变长整数");value|=uint64_t(c&127)<<(i*7);
            if(!(c&128)){add(at,pos-at,name,"ULEB128 / u64",std::to_string(value),evidence,known);return value;}}
        throw std::runtime_error("无效的 64 位变长整数");
    }
    uint32_t count(const std::string& name,const std::string& evidence,size_t minimumBytes) {
        auto n=var(name,evidence);if(n>4096 || n>(b.size()-pos)/minimumBytes)throw std::runtime_error("数组数量超过剩余字节或分析限制");return n;
    }
    void stringBytes(const std::string& name,const std::string& evidence,bool known=false) {
        auto n=var(name+" 字节数",evidence);need(n);auto at=pos;pos+=n;
        auto bytes=b.subspan(at,n);auto text=utf8Preview(bytes);
        add(at,n,name,text?"UTF-8":"invalid UTF-8 / bytes",text?*text:hex(bytes.first(std::min<size_t>(n,128))),evidence+"；UTF-8 读取链 0x9330FD0 → 0x3E87B40 → 0x3EAE8B0；原始字节保留",known);
    }
    void blob(const std::string& name,const std::string& evidence) {
        auto n=var(name+" 字节数",evidence);need(n);auto at=pos;pos+=n;
        add(at,n,name,"bytes",hex(b.subspan(at,n).first(std::min<size_t>(n,128))),evidence+"；内部结构待确认，预览限长，原始字节保留",false);
    }
    std::string member(const std::string& prefix,unsigned offset) {std::ostringstream s;s<<prefix<<" +0x"<<std::hex<<std::uppercase<<offset<<"（语义待确认）";return s.str();}
    void raw(const std::string& prefix,unsigned offset,size_t n,const std::string& evidence){u(member(prefix,offset),n,evidence,false);}
    void rawVar(const std::string& prefix,unsigned offset,const std::string& evidence){var(member(prefix,offset),evidence,false);}
    void raw64(const std::string& prefix,unsigned offset,const std::string& evidence){var64(member(prefix,offset),evidence);}
    void rawBool(const std::string& prefix,unsigned offset,const std::string& evidence){boolean(member(prefix,offset),evidence);}
    void flagsKnown(uint64_t flags,uint64_t mask){if(flags&~mask)throw std::runtime_error("出现未定义的可选字段位");}
    void f32(const std::string& name,const std::string& evidence,bool known=true) {
        need(4);auto at=pos;float v=std::bit_cast<float>(uint32_t(readInteger(b,pos,4,false)));pos+=4;
        add(at,4,name,"float32 LE",number(v),evidence,known);
    }
    void angle(const std::string& name,const std::string& evidence,bool known) {
        auto v=u(name,2,evidence,known);out.fields.back().value += " / "+number(double(v)*360.0/65536.0)+"°";
    }
    void floatEncodedAngle(const std::string& name,const std::string& evidence) {
        const auto at=pos;f32(name,evidence);
        const auto v=std::bit_cast<float>(uint32_t(readInteger(b,at,4,false)));
        // Native cvttss2si, then low 16 bits. Reject nonfinite/out-of-range
        // input before conversion to avoid undefined behavior in C++.
        if(!std::isfinite(v) || double(v)<-2147483648.0 || double(v)>=2147483648.0){
            out.fields.back().meaningKnown=false;
            throw std::runtime_error("朝向值超出可转换范围");
        }
        auto encoded=static_cast<uint16_t>(static_cast<int32_t>(v));
        out.fields.back().value += " / "+number(double(encoded)*360.0/65536.0)+"°";
    }
    void varAngle(const std::string& name,const std::string& evidence) {
        const auto at=pos;uint32_t value=0;
        for(unsigned i=0;i<3;++i){
            need(1);auto c=b[pos++];
            if(i==2 && (c&0xfc))throw std::runtime_error("无效的 16 位变长角度");
            value|=uint32_t(c&127)<<(i*7);
            if(!(c&128)){
                add(at,pos-at,name,"ULEB128 / u16",std::to_string(value)+" / "+number(double(value)*360.0/65536.0)+"°",evidence,true);
                return;
            }
        }
        throw std::runtime_error("无效的 16 位变长角度");
    }
    void boolean(const std::string& name,const std::string& evidence,bool known=false) {
        if(boolBit==8){need(1);boolPos=pos++;boolBit=0;}
        add(boolPos,1,name,"bit",(b[boolPos]&(1u<<boolBit))?"true":"false",evidence,known,int(boolBit));++boolBit;
    }
};
void moveStateField(Reader& r,const std::string& ev) {
    static constexpr const char* states[]={"Stop","Run","Walk","Parkour","Rolling","Evade","Gravity","FlyingOnAirStop","Flying","FlyingWalk","Glide","WindPath","SwimmingStop","Swimming","SwimmingWalk","SwimmingDive"};
    auto state=r.u("移动状态",1,ev+"；_move_state / MoveState 0xEE2D080",false);
    if(state<std::size(states)){r.out.fields.back().value+=" / "+std::string(states[state]);r.out.fields.back().meaningKnown=true;}
}
void localMovementFields(Reader& r,uint16_t opcode) {
    const bool stopped=opcode==0x371A,ground=opcode>=0x371A && opcode<=0x371D;
    const bool delta=opcode==0x371D || opcode==0x3720,attack=opcode==0x372E || opcode==0x372F;
    const std::string ev=std::string("局部移动读取 ")+(stopped?"0x85796C0":attack?"0x85792F0":ground?(delta?"0x85791A0":"0x8578E60"):(delta?"0x8579550":"0x8578F70"))+"；反射 0xEE20A58..0xEE20BA8";
    const auto flags=delta?r.u("局部坐标位图",1,ev):0;r.flagsKnown(flags,7);
    r.u("局部载体实体编号",4,ev+"；_vessel_key，0 表示无载体");
    if(delta){
        for(unsigned i=0;i<3;++i)if(flags&(uint64_t{1}<<i))r.i8(std::string("局部 delta ")+"XYZ"[i],ev+"；_offset_x/y/z，有符号局部位置增量");
    }else for(auto axis:{"X","Y","Z"})r.f32(std::string("局部位置 ")+axis,ev+"；_pos，不是世界位置");
    if(attack)for(auto axis:{"X","Y","Z"})r.f32(std::string("局部目标位置 ")+axis,ev+"；_target_pos，参考系细节待确认");
    if(!ground)for(auto axis:{"X","Y","Z"})r.f32(std::string("局部移动速度 ")+axis,ev+"；_move_velocity_3d，单位待确认");
    if(ground && !stopped)r.varAngle("局部移动方向",ev+"；_move_dir，0x9330D50 读取 u16 变长整数");
    r.varAngle("局部朝向 yaw",ev+"；_rotate_dir，0x9330D50 读取 u16 变长整数；消费者 ×360/65536");
}
void identityFields(Reader& r,const std::string& p) {
    const std::string ev="读取 0x855B430；CharDesc 0xEE1DE28 / 属性 0xDC91690";auto f=r.u(p+" 位图",1,ev);r.flagsKnown(f,31);
    auto enumeration=[&](const char* name,std::initializer_list<const char*> names,const char* descriptor){
        auto v=r.u(p+" "+name,1,ev+"；"+descriptor,false);
        if(v<names.size()){r.out.fields.back().value+=" / "+std::string(*(names.begin()+v));r.out.fields.back().meaningKnown=true;}
    };
    if(f&1)r.stringBytes(p+" 角色昵称",ev+"；_nickname +8",true);r.u(p+" PC ID",4,ev+"；_pc_id +0x18");
    if(f&2)enumeration("种族",{"None","Light","Dark","All","Max"},"ERace 0xEE2B970 / +0x1C");
    if(f&4)enumeration("性别",{"None","Male","Female","Max"},"EGender 0xEE2D620 / +0x1D");
    if(f&8){enumeration("匿名类型",{"None","AnonymousBot","Mercenary","Max"},"AnonymousData 0xEE1DDB8；EAnonymousType 0xEE2DC50 / +0x28");r.u(p+" 匿名索引",4,ev+"；AnonymousData._index +0x2C");}
    if(f&16)enumeration("覆盖种族",{"None","Light","Dark","All","Max"},"ERace 0xEE2B970 / +0x30");
}
void stateFields(Reader& r,const std::string& p) {
    const std::string ev="读取 0x855D980；CharState 0xEE1DE98 / 属性 0xDC8FB80";auto f=r.u(p+" 位图",2,ev);
    auto vec=[&](const char* name,const char* member){for(auto axis:{"X","Y","Z"})r.f32(p+" "+name+" "+axis+"（单位待确认）",ev+"；"+member+"；Vec3 0xEE1DB18 的 _x/_y/_z 均为 float",false);};
    vec("位置","_pos +8");r.f32(p+" 朝向（单位待确认）",ev+"；_dir +0x20",false);r.u(p+" 移动方向（编码待确认）",2,ev+"；_move_dir +0x24，线上为 u16",false);
    if(f&1)vec("局部位置","_pos_local +0x28");
    if(f&2)r.f32(p+" 局部朝向（单位待确认）",ev+"；_dir_local +0x40",false);if(f&4)r.u(p+" 局部移动方向（编码待确认）",2,ev+"；_move_dir_local +0x44，线上为 u16",false);
    if(f&8)vec("移动速度","_move_velocity_3d +0x48");
    if(f&16)vec("局部移动速度","_move_velocity_3d_local +0x60");
    if(f&32)r.u(p+" 目标实体键",4,ev+"；_target_key +0x78");r.u(p+" 存活状态（取值待确认）",1,ev+"；_live +0x7C",false);
    const std::string hpEv=ev+"；3641: 0x9037DFF..0x9037E24 / 3645: 0x9036741..0x9036756 → EResource 1=HP, 9=HP_Max";
    r.var64(p+" HP",hpEv,true);r.var64(p+" HP 上限",hpEv,true);
    for(auto name:{"MP","MP 上限","AP","AP 上限","SP","SP 上限","DP","DP 上限","DP step","OP","OP 上限","FP","FP 上限"})r.u(p+" "+name,4,ev+"；+0x90..+0xC0 依次对应 _mp/_mp_max/_ap/_ap_max/_sp/_sp_max/_dp/_dp_max/_dp_step/_op/_op_max/_fp/_fp_max；保留资源原缩写");
    if(f&64)r.u(p+" 战斗状态（取值待确认）",1,ev+"；_combat_state +0xC4",false);
    if(f&128)r.u(p+" 开关标志（位含义待确认）",4,ev+"；_toggle_flags +0xC8",false);
    if(f&256)r.u(p+" 阶段（取值待确认）",1,ev+"；_phase +0xCC",false);
    if(f&512){
        static constexpr const char* names[]={"NoRelation","PC_Light_Common","PC_Darkness_Common","NPC_Light","NPC_Dark","NPC_Neutral","Monster","EnvObj_Neutral","EnvObj_Light","EnvObj_Dark","Rift","Monster_AntiCompanion","Monster_PCFriendly","NPC_Companion","NPC_NoInteraction","EnvObj_Destroy","Monster_TeamA","Monster_TeamB","Monster_TeamC","Monster_TeamD","Monster_TeamE","Monster_Invisible","PC_Summon","PC_FriendlyToAll","PC_RoleA_Common","PC_RoleB_Common","EnvObj_RoleA","EnvObj_RoleB","EnvObj_MonsterEnemy","Pc_FreeForAll_Common","PC_EnemyToAll","PC_Light_Friendly","PC_Darkness_Friendly","PC_Light_RiftEvent","PC_Darkness_RiftEvent","Monster_Light_RiftEvent","Monster_Dark_RiftEvent","EnvObj_Light_RiftEvent","EnvObj_Dark_RiftEvent","NPC_Light_Guard","NPC_Dark_Guard","Max"};
        auto v=r.u(p+" 关系实体类别",1,ev+"；_relationship_entity +0xCD；ERelationshipEntity 0xEE2C0F0",false);
        if(v<std::size(names)){r.out.fields.back().value+=" / "+std::string(names[v]);r.out.fields.back().meaningKnown=true;}
    }
    if(f&1024)r.var(p+" 锁定目标实体键",ev+"；_lockon_target_key +0xD0");
    if(f&2048)r.u(p+" 锁定目标部位 ID",4,ev+"；_lockon_target_parts_id +0xD4");
    if(f&4096)r.u(p+" 翅膀 ID",4,ev+"；_wing_id +0xD8");
    if(f&8192)r.u(p+" 翅膀等级",1,ev+"；_wing_level +0xDC");
    if(f&16384)r.u(p+" 翅膀外观 ID",4,ev+"；_wing_skin_id +0xE0");
    if(f&32768)r.u(p+" 称号 ID",4,ev+"；_title_id +0xE4");
}
void initialStatFields(Reader& r,const std::string& ev) {
    auto n=r.count("属性条目数量",ev,6);
    for(uint32_t i=0;i<n;++i){auto p="属性条目 "+std::to_string(i);auto key=r.u(p+" 编号",2,ev+"；EStat 表 0xDD58940",false);
        if(key<statNames.size()){r.out.fields.back().value+=" / "+std::string(statNames[key]);r.out.fields.back().meaningKnown=true;p+=" "+std::string(statNames[key]);}
        r.i32(p+" 原始值（单位/缩放待确认）",ev+" → 0x8B454C0，i32 符号扩展后提交",false);}
}
void effectFields(Reader& r,const std::string& p) {
    const std::string ev="读取 0x855E240；Abnormal 0xEE1DFB0 / 属性 0xDC901D0";auto f=r.u(p+" 位图",1,ev);r.flagsKnown(f,31);
    r.var(p+" 状态实例编号",ev+"；_uid +8");r.u(p+" 异常状态 ID",4,ev+"；_abnormal_id +0xC");
    r.u(p+" 持续毫秒数",8,ev+"；_duration_ms +0x10");
    r.u(p+" 到期 Unix 毫秒（0 未设置）",8,ev+"；_expire_time +0x18；0x90559B0 将消息 +0x30 按完整 64 位乘 10000 加 621355968000000000，0 保持未设置");
    if(f&1)r.var(p+" 施放者实体键",ev+"；_caster_key +0x20");
    r.u(p+" 异常状态等级",1,ev+"；_abnormal_level +0x24");if(f&2)r.u(p+" 技能 ID",4,ev+"；_skill_id +0x28");
    r.boolean(p+" 待移除",ev+"；_ready_to_remove；赋值函数 0x617D350 写 +0x2C",true);
    if(f&4)r.var(p+" multiply 施放者实体键",ev+"；_multiply_caster_key +0x30；保留原命名，不推断叠加机制");
    if(f&8)r.u(p+" multiply_value（用途待确认）",4,ev+"；_multiply_value +0x34",false);
    if(f&16)for(auto axis:{"X","Y","Z"})r.f32(p+" 范围基准 "+axis+"（单位待确认）",ev+"；_range_base +0x38 → Vec3 0xEE1DB18 的 _x/_y/_z",false);
}
void affiliationFields(Reader& r,const std::string& p) {
    const std::string ev="IDA 0x85622A0 / 0x90D4DF0 → Object_GuildChanged；0x8DAE840 公会名称缓存";
    r.u(p+" 公会编号",8,ev);r.stringBytes(p+" 公会名称",ev,true);
    const auto meta=ev+"；GuildDesc 0xEE1E480 属性序列与 0x85622A0 对应，反射布局不代替线上宽度";
    r.u(p+" 公会徽章 ID",2,meta);r.u(p+" 成员级别（枚举待确认）",1,meta,false);r.u(p+" 公会排名",4,meta);
    r.u(p+" 联盟编号",8,meta);r.u(p+" 联盟徽章 ID",2,meta);r.stringBytes(p+" 联盟名称",meta,true);
}
void optionalFields(Reader& r,const std::string& p) {
    const std::string ev="IDA 0x857A6B0";auto f=r.u(p+" 位图",1,ev);r.flagsKnown(f,63);r.raw(p,8,1,ev);
    if(f&1)r.raw(p,0xc,4,ev);if(f&2)r.raw(p,0x10,4,ev);if(f&4)r.raw(p,0x14,4,ev);if(f&8)r.raw(p,0x18,8,ev);if(f&16)r.raw(p,0x20,8,ev);r.rawBool(p,0x28,ev);if(f&32)r.raw(p,0x30,8,ev);r.rawBool(p,0x38,ev);
}
bool skinBool(Reader& r,const std::string& p,const std::string& ev){r.boolean(p,ev,true);return r.out.fields.back().value=="true";}
std::vector<SkinDye> skinDyeFields(Reader& r,const std::string& p){
    const std::string ev="2026-09-30 SkinDecoInfo 原生读取器 / 反射核对";
    auto n=r.count(p+" 染色槽数量",ev,28);std::vector<SkinDye> dyes;
    for(uint32_t i=0;i<n;++i){
        SkinDye d;auto q=p+" 染色 "+std::to_string(i);
        d.slot=uint8_t(r.u(q+" 槽",1,ev));
        for(size_t a=0;a<3;++a)d.rgb[a]=uint8_t(r.u(q+" "+std::string(1,"RGB"[a]),1,ev));
        d.remove=skinBool(r,q+" 移除染色",ev);
        d.materialId=uint32_t(r.u(q+" 材质 ID",4,ev));d.patternId=uint32_t(r.u(q+" 图案 ID",4,ev));
        for(size_t a=0;a<3;++a)d.patternRgb[a]=uint8_t(r.u(q+" 图案 "+std::string(1,"RGB"[a]),1,ev));
        d.patternRemove=skinBool(r,q+" 移除图案染色",ev);
        d.patternColorOpacity=uint16_t(r.u(q+" 图案颜色不透明度（原值）",2,ev));
        d.patternOpacity=uint16_t(r.u(q+" 图案不透明度（原值）",2,ev));
        d.sizeH=uint16_t(r.u(q+" 图案尺寸 H（原值）",2,ev));d.sizeV=uint16_t(r.u(q+" 图案尺寸 V（原值）",2,ev));
        d.patternType=uint8_t(r.u(q+" 图案类型",1,ev,false));d.patternAdditional=skinBool(r,q+" 图案 additional",ev);
        d.glossy=uint16_t(r.u(q+" 光泽（原值）",2,ev));d.metallic=uint16_t(r.u(q+" 金属感（原值）",2,ev));dyes.push_back(d);
    }return dyes;
}
void skinDisplayFields(Reader& r,SkinEquipment& s,const std::string& p,const std::string& ev){
    s.display=skinBool(r,p+" 显示",ev);s.defaultMesh=skinBool(r,p+" 默认模型标志",ev);s.overrideMesh=skinBool(r,p+" 覆盖标志",ev);
}
void equipmentFields(Reader& r,const std::string& p) {
    const std::string ev="EquipBroad 2026-09-30 0x148489AF0 / 旧版 0x8558EF0";SkinEquipment s;
    s.itemId=uint32_t(r.u(p+" 装备 ID",4,ev));r.u(p+" 强化等级",1,ev);s.visualSlot=uint8_t(r.u(p+" 显示部位",1,ev));
    r.u(p+" 到期时间",8,ev);r.boolean(p+" 已到期",ev,true);s.skinId=uint32_t(r.u(p+" 时装 ID",4,ev));
    s.dyes=skinDyeFields(r,p);
    auto n=r.count(p+" 神石数量",ev,4);for(uint32_t i=0;i<n;++i)r.u(p+" 神石["+std::to_string(i)+"]",4,ev);
    skinDisplayFields(r,s,p,ev);r.var(p+" 特效位",ev,false);r.out.skins.push_back(std::move(s));
}
void viewCharEquipmentFields(Reader& r){
    const std::string ev="ViewCharEquipItemInfo 0x1484A1270 / EquipItemDetail 0x148490170（2026-09-30）";
    auto n=r.count("资料装备数量",ev,50);std::vector<SkinEquipment> skins;
    for(uint32_t i=0;i<n;++i){
        SkinEquipment s;auto p="资料装备 "+std::to_string(i);
        s.itemId=uint32_t(r.u(p+" 装备 ID",4,ev));r.stringBytes(p+" 制作者",ev,true);r.u(p+" 到期时间",8,ev);r.boolean(p+" 已到期",ev,true);
        for(auto width:{1,4,4,1,4,4})r.u(p+" 装备养成原值",width,ev,false);
        r.boolean(p+" 突破",ev,true);r.u(p+" 装备槽",1,ev);
        auto arr=[&](std::initializer_list<size_t> widths){size_t minimum=0;for(auto w:widths)minimum+=w;auto k=r.count(p+" 详情数组数量",ev,minimum);for(uint32_t j=0;j<k;++j)for(auto w:widths)r.u(p+" 详情数组原值",w,ev,false);};
        arr({4,2,1});r.u(p+" 绑定角色 ID",8,ev);r.u(p+" 绑定次数",1,ev);arr({4});
        arr({2,4});arr({2,4});arr({2,4});arr({4,1});arr({4,1});arr({2,4});arr({4,1});r.u(p+" 外观提取次数",1,ev);
        s.visualSlot=uint8_t(r.u(p+" 显示部位",1,ev));s.skinId=uint32_t(r.u(p+" 时装 ID",4,ev));s.dyes=skinDyeFields(r,p);
        skinDisplayFields(r,s,p,ev);skins.push_back(std::move(s));
    }
    r.out.skins=std::move(skins);r.out.skinsComplete=true;
}
void selfDetailFields(Reader& r) {
    const std::string ev="读取 0x855C1B0；CharDetail 0xEE1DE60（镜像字段按读取顺序对应）",p="基础结构 ";
    auto f=r.u(p+"位图",1,ev);r.flagsKnown(f,63);
    auto en=[&](const std::string& name,std::initializer_list<const char*> names){auto n=r.u(p+name,1,ev,false);if(n<names.size()){r.out.fields.back().value+=" / "+std::string(*(names.begin()+n));r.out.fields.back().meaningKnown=true;}};
    if(f&1)r.stringBytes(p+"角色昵称",ev,true);r.u("服务器 ID",2,ev+"；_server_id 原生 +0x18 为 u16");r.u(p+"PC ID",4,ev);
    if(f&2)en("种族",{"None","Light","Dark","All","Max"});
    r.u("等级",4,ev+"；_char_level +0x24");r.u("装备等级",4,ev+"；_equip_item_level +0x28");
    r.u("历史最高总装备等级",4,ev+"；_highest_total_item_level");r.u("等级上限",4,ev+"；_level_max");r.u("经验",8,ev+"；_exp");
    r.u("战斗等级",4,ev+"；_combat_level");r.u("战斗经验",8,ev+"；_combat_exp");if(f&4)en("性别",{"None","Male","Female","Max"});
    auto n=r.count("采集属性数量",ev,14);
    for(uint32_t i=0;i<n;++i){auto s="采集属性 "+std::to_string(i)+" ";r.u(s+"等级",2,ev);r.u(s+"经验",8,ev);r.u(s+"技能点",2,ev);r.u(s+"专精点",2,ev);}
    n=r.count("制作属性数量",ev,13);
    for(uint32_t i=0;i<n;++i){auto s="制作属性 "+std::to_string(i)+" ";r.u(s+"类别（枚举待确认）",1,ev,false);r.u(s+"等级",2,ev);r.u(s+"经验",8,ev);r.u(s+"技能点",2,ev);}
    r.u("滑翔冷却（毫秒）",4,ev+"；_glide_cooltime；0x903582A → 0x8E37650，乘 10000 加日期 tick");r.u("冲刺冷却（毫秒）",4,ev+"；_sprint_cooltime；0x9035842 → 0x8E35460，乘 10000 加日期 tick");r.u("声望",8,ev+"；_reputation");
    r.u("ascension（枚举待确认）",1,ev,false);if(f&8)en("覆盖种族",{"None","Light","Dark","All","Max"});
    for(unsigned bit=4;bit<6;++bit)if(f&(uint64_t{1}<<bit)){auto s=bit==4?"技能点":"额外技能点";n=r.count(std::string(s)+"数量",ev,3);for(uint32_t i=0;i<n;++i){auto label=std::string(s)+" "+std::to_string(i);r.u(label+" 类型（枚举待确认）",1,ev,false);r.u(label+" 点数",2,ev);}}
}
void selfAppearFields(Reader& r) {
    const std::string ev="读取 0x92BC920；MyCharAppear_NT 0xEDF8CB0 → 0x9034770";
    r.var("实体编号",ev);auto f=r.u("可选字段位图",4,ev);r.flagsKnown(f,0x3fffffff);
    selfDetailFields(r);stateFields(r,"状态结构");if(f&1)affiliationFields(r,"关联结构");initialStatFields(r,ev);
    auto n=r.count("附加状态数量",ev,23);for(uint32_t i=0;i<n;++i)effectFields(r,"附加状态 "+std::to_string(i));
    n=r.count("装备数量",ev,21);for(uint32_t i=0;i<n;++i)equipmentFields(r,"装备 "+std::to_string(i));
    if(f&2)r.u("出现原因（枚举待确认）",1,ev,false);r.u("地图实例键",8,ev+"；_map_key");r.u("地图 ID",4,ev+"；_map_id");
    if(f&4)r.u("子区域 ID",4,ev);if(f&8)r.u("先前子区域 ID",4,ev);
    if(f&16)for(auto a:{"X","Y","Z"})r.f32(std::string("先前位置 ")+a,ev+"；Vec3 0x85583E0，单位待确认",false);
    if(f&32)r.u("频道编号",2,ev+"；_channel_no 原生 +0x260 为 u16");r.u("角色数据库 ID",8,ev+"；_dbid +0x268");
    if(f&64)r.f32("最近地面位置 Z",ev+"；_last_on_ground_pos_z，单位待确认",false);if(f&128)r.u("天气 ID",4,ev);
    if(f&256)r.u("天气类型（枚举待确认）",1,ev,false);if(f&512)r.u("移动状态（枚举待确认）",1,ev,false);if(f&1024)r.u("移动子状态（枚举待确认）",1,ev,false);
    if(f&2048)r.boolean("启用航位推算",ev+"；_dead_reckoning_on",true);if(f&4096)r.var("传送效果 ID",ev+"；_teleport_effect_id");
    r.blob("外观定制数据",ev+"；_customizing_data");r.boolean("已购买外观定制",ev+"；_is_purchased_customizing",true);
    if(f&8192)r.u("声音类型（枚举待确认）",1,ev,false);if(f&16384)r.var("空域点数",ev+"；_airspace_point");if(f&32768)r.u("封印石物品 ID",4,ev);
    if(f&65536)r.u("深渊排名点数",8,ev);
    if(f&0x20000){n=r.count("地图标记数量",ev,35);for(uint32_t i=0;i<n;++i){auto p="地图标记 "+std::to_string(i)+" ";auto mf=r.u(p+"位图",1,ev);r.flagsKnown(mf,1);
        r.u(p+"索引",1,ev);r.stringBytes(p+"名称",ev,true);r.u(p+"地图 ID",4,ev);for(auto a:{"X","Y","Z"})r.f32(p+"位置 "+a,ev+"；MapPinInfo 0xEE1EB48",false);
        r.u(p+"更新时间（单位待确认）",8,ev,false);r.u(p+"角色数据库 ID",8,ev);if(mf&1){auto k=r.count(p+"移除成员数量",ev,8);for(uint32_t j=0;j<k;++j)r.u(p+"移除成员 "+std::to_string(j),8,ev);}}}
    if(f&0x40000)r.u("坐骑数据 ID",4,ev);if(f&0x80000)r.u("arcana_extract_point",4,ev+"；保留原命名",false);if(f&0x100000)r.u("arcana_extract_point_2",4,ev+"；保留原命名",false);
    if(f&0x200000)r.u("arcana_create_end_time（单位待确认）",8,ev,false);
    if(f&0x400000){r.u("上次死亡记录时间（单位待确认）",8,ev+"；LastDeathInfo 0x856F860",false);r.u("击杀者服务器 ID",2,ev);r.stringBytes("击杀者昵称",ev,true);r.u("击杀怪物数据 ID",4,ev);r.u("死亡类型（枚举待确认）",1,ev,false);}
    if(f&0x800000)r.u("匹配服务器 ID",2,ev);
    r.u("征服者等级",4,ev);r.u("征服者等级上限",4,ev);r.u("征服者经验",8,ev);
    if(f&0x1000000)r.boolean("裂缝 PvP 开启",ev,true);if(f&0x2000000)r.u("裂缝 PvP 设置时间（单位待确认）",8,ev,false);if(f&0x4000000)r.u("变形状态实例 UID",4,ev);
    if(f&0x8000000)for(auto name:{"传承次数","高级传承次数","恢复传承次数","恢复高级传承次数"})r.u(name,4,ev+"；SuccessionCountInfo 0x8584260");
    r.u("战斗力",8,ev);r.u("历史最高战斗力",8,ev);
    for(auto name:{"已用 Daevanion 点数","额外 Daevanion 点数"}){n=r.count(std::string(name)+"数量",ev,3);for(uint32_t i=0;i<n;++i){auto p=std::string(name)+" "+std::to_string(i);r.u(p+" 类型（枚举待确认）",1,ev,false);r.u(p+" 点数",2,ev);}}
    if(f&0x10000000){n=r.count("试炼完成条目数量",ev,8);for(uint32_t i=0;i<n;++i){auto p="试炼完成 "+std::to_string(i);r.u(p+" 副本 ID",4,ev);r.u(p+" 完成等级",4,ev);}}
    if(f&0x20000000){n=r.count("角色投票计数数量",ev,8);for(uint32_t i=0;i<n;++i){auto p="角色投票计数 "+std::to_string(i);r.u(p+" 键（含义待确认）",4,ev,false);r.u(p+" 次数",4,ev);}}
}
void optionalFieldsB(Reader& r,const std::string& p) {
    const std::string ev="IDA 0x857AAE0";auto f=r.u(p+" 位图",1,ev);r.flagsKnown(f,15);r.raw(p,8,1,ev);r.raw(p,0xc,4,ev);r.raw(p,0x10,1,ev);r.raw(p,0x11,1,ev);r.rawBool(p,0x12,ev);r.raw(p,0x14,4,ev);
    if(f&1)r.raw(p,0x18,8,ev);if(f&2)r.raw(p,0x20,4,ev);if(f&4)r.raw(p,0x28,8,ev);if(f&8)r.raw(p,0x30,4,ev);
}
void npcAppearFields(Reader& r,bool current) {
            const std::string ev=current?"2026-09-30 IDA RVA 0x91E1D50 / NpcAppear_NT":"2026-09-24 IDA RVA 0x92BF830 / NpcAppear_NT",p="对象";r.var("实体编号",ev);auto f=r.u("可选字段位图",current?2:4,ev);r.flagsKnown(f,current?0xffff:0x1ffff);
            identityFields(r,"基础结构");stateFields(r,"状态结构");auto n=r.count("附加状态数量",ev,23);for(uint32_t i=0;i<n;++i)effectFields(r,"附加状态 "+std::to_string(i));
            if(f&1)r.raw(p,0x148,1,ev);if(f&2)r.raw(p,0x149,1,ev);if(f&4)r.raw(p,0x14a,1,ev);if(f&8)r.rawBool(p,0x14b,ev);if(f&16)r.raw(p,0x14c,4,ev);
            if(f&32)identityFields(r,"基础结构 B");if(f&64)affiliationFields(r,"关联结构");
            initialStatFields(r,ev+" / 0x9037DD0..0x9037DEB");
            r.u("等级",4,ev+"；_char_level +0x1E0");if(f&128)r.raw(p,0x1e8,8,ev);if(f&256)r.raw(p,0x1f0,4,ev);if(f&512)optionalFields(r,"可选结构");
            if(f&1024){r.raw(p,0x240,4,ev);r.raw(p,0x244,1,ev);r.raw(p,0x248,4,ev);}if(f&2048)r.raw(p,0x250,8,ev);if(f&4096)r.raw(p,0x258,4,ev);if(f&8192)r.raw(p,0x25c,4,ev);
            n=r.count("尾部数组数量",ev,4);for(uint32_t i=0;i<n;++i)r.u("尾部数组["+std::to_string(i)+"]",4,ev,false);
            if(f&16384)r.raw(p,0x270,8,ev);if(f&32768)r.raw(p,0x278,4,ev);if(!current && (f&65536))r.raw(p,0x280,8,ev);
}
void worldLoginEntry(Reader& r,const std::string& p) {
    const std::string ev="IDA 0x855A2D0";
    r.u(p+" 角色列表编号",4,ev+"；0x8FEF190 按条目 +8 建表，0x8FED950 查找；0x90DECB0 / 0x8FF0E30 按此编号改名，与场景实体编号分开");
    r.stringBytes(p+" 角色名",ev+"；0x8FEFDA0 保持 +0x10；改名处理 0x8FF0EFC 更新 +0x10，触发 Lobby_CharacterNameChanged",true);
    r.u(p+" PcData 配置编号",4,ev+"；0x8FEF2D7 取 +0x20，交给 0x8AED180 / 0x9883DE0 查询；虚表 0xDE9FE20 名称函数 0x8AB9AE0 返回 PcData");
    r.raw(p,0x24,1,ev);r.raw(p,0x25,1,ev);
    for(auto o:{0x28,0x2c,0x30})r.raw(p,o,4,ev);r.raw(p,0x38,8,ev);r.raw(p,0x40,4,ev);r.raw(p,0x48,8,ev);
    r.stringBytes(p+" 字符串 B",ev);r.raw(p,0x60,8,ev);for(auto o:{0x70,0x74,0x78,0x80})r.raw(p,o,4,ev);
    r.raw(p,0x88,8,ev);r.raw(p,0x90,8,ev);
    auto n=r.count(p+" 复合条目数量",ev,21);for(uint32_t i=0;i<n;++i)equipmentFields(r,p+" 复合条目 "+std::to_string(i));
    r.raw(p,0xa8,8,ev);r.raw(p,0xb0,4,ev);r.raw(p,0xb8,8,ev);r.raw(p,0xc0,4,ev);r.raw(p,0xc4,4,ev);
    n=r.var(p+" 字节数组长度",ev);r.need(n);
    r.add(r.pos,n,p+" 字节数组（内部结构待确认）","bytes",hex(r.b.subspan(r.pos,n).first(std::min<size_t>(n,128))),ev+"；预览限长，原始字节保留",false);r.pos+=n;
    r.raw(p,0xd8,1,ev);r.rawBool(p,0xd9,ev);
    n=r.count(p+" 尾部条目数量",ev,20);
    for(uint32_t i=0;i<n;++i){auto q=p+" 尾部条目 "+std::to_string(i);r.raw(q,8,4,ev);r.raw(q,0x10,8,ev);r.rawBool(q,0x18,ev);r.raw(q,0x20,8,ev);}
    r.raw(p,0xf0,8,ev);r.raw(p,0xf8,1,ev);r.raw(p,0x100,8,ev);
}
bool loginFields(Reader& r,uint64_t opcode) {
    auto result=[&](const std::string& ev){r.u("结果码（0=成功；其他值待确认）",2,ev,false);};
    switch(opcode) {
    case 0x3901: {
        const std::string ev="登录分派 0x92B6170 / 读取 0x9327990";r.out.name+=" 登录握手响应";result(ev);
        r.u("握手附加值（语义待确认）",2,ev,false);auto n=r.var("握手数据长度",ev);r.need(n);
        r.add(r.pos,n,"握手数据","bytes",hex(r.b.subspan(r.pos,n).first(std::min<size_t>(n,64))),"0x90ECC70 将该数据转换后传入 0x9204620；内部结构待确认",false);r.pos+=n;
        r.stringBytes("握手字符串",ev);r.boolean("握手标志（语义待确认）",ev);r.raw("握手对象",0x2c,4,ev);r.raw("握手对象",0x30,4,ev);return true;
    }
    case 0x3903:
        r.out.name+=" 登录时间响应";result("IDA 0x92B62AE");r.u("时间值 A（时基待确认）",8,"IDA 0x92B62C0",false);r.u("时间值 B（时基待确认）",8,"IDA 0x92B62D2",false);return true;
    case 0x3906: {
        const std::string ev="登录响应读取 0x9327E00 / 处理 0x92B67E0";r.out.name+=" 登录响应";result(ev);
        r.stringBytes("登录字符串 A",ev);r.stringBytes("登录字符串 B",ev);r.raw("登录对象",0x28,1,ev);r.raw("登录对象",0x2a,2,ev);
        r.stringBytes("登录字符串 C",ev);r.raw("登录对象",0x40,2,ev);r.rawBool("登录对象",0x42,ev);r.rawBool("登录对象",0x43,ev);
        r.stringBytes("登录字符串 D",ev);r.raw("登录对象",0x58,1,ev);for(auto o:{0x5a,0x5c,0x5e})r.raw("登录对象",o,2,ev);r.raw("登录对象",0x60,1,ev);return true;
    }
    case 0x3909: {
        const std::string ev="登录服务器列表 0x92B6170 / 读取 0x9328090";r.out.name+=" 登录服务器列表";result(ev);auto n=r.count("服务器数量",ev,18);
        for(uint32_t i=0;i<n;++i){auto p="服务器 "+std::to_string(i);r.u(p+" 编号",2,ev+"；0x92B6BE4 按此编号查找");r.raw(p,0xa,1,ev);r.raw(p,0xc,4,ev);r.stringBytes(p+" 名称字节",ev);
            r.rawBool(p,0x20,ev);r.raw(p,0x21,1,ev);for(auto o:{0x22,0x24,0x26})r.raw(p,o,2,ev);r.rawBool(p,0x28,ev);r.raw(p,0x29,1,ev);r.raw(p,0x2a,2,ev);}
        r.raw("列表",0x18,1,ev);r.raw("列表",0x19,1,ev);return true;
    }
    case 0x390B: {
        const std::string ev="登录角色列表 0x92B6170 / 读取 0x9328420";r.out.name+=" 登录角色列表";result(ev);auto n=r.count("角色条目数量",ev,36);
        for(uint32_t i=0;i<n;++i){auto p="角色 "+std::to_string(i);r.raw(p,8,2,ev);r.stringBytes(p+" 字符串",ev);r.raw(p,0x20,4,ev);r.raw(p,0x28,8,ev);r.raw(p,0x30,4,ev);r.raw(p,0x34,1,ev);r.raw(p,0x38,8,ev);r.rawBool(p,0x40,ev);r.raw(p,0x48,8,ev);r.rawBool(p,0x50,ev);}
        r.raw("列表",0x18,1,ev);r.raw("列表",0x19,1,ev);return true;
    }
    case 0x390D:
        r.out.name+=" 选择服务器响应";result("IDA 0x92B6AE0");r.u("选择状态（枚举待确认）",1,"IDA 0x92B6B28",false);r.u("服务器编号",2,"IDA 0x92B6B3B / 0x92B6BE4");return true;
    case 0x390F:
        r.out.name+=" 转服地址响应";result("IDA 0x92B6CB0");r.u("附加编号（语义待确认）",2,"IDA 0x92B6CF9",false);r.stringBytes("目标服务器地址","IDA 0x92B6D06 / 0x8FAD1D0");r.u("目标端口",2,"IDA 0x92B6D19 / 0x8FAD1D0");return true;
    default:return false;
    }
}
}
std::string gameQueryResultText(uint16_t result){
    const char* meaning="未知错误（暂未确认含义）";const char* symbol="";
    // EResult table in 2026-09-30 dump, 0x14DD41BE0..0x14DD41C50.
    switch(result){
    case 0:meaning="成功";break;
    case 6432:meaning="角色不存在";symbol="kCharacter_NotExist";break;
    case 6433:meaning="角色名称无效";symbol="kCharacter_InvalidName";break;
    case 6434:meaning="角色受限（具体限制待确认）";symbol="kCharacter_Blocked";break;
    case 6435:meaning="查看角色资料过于频繁";symbol="kCharacter_RequetCharacterViewTooSoon";break;
    case 6436:meaning="占位角色（具体原因待确认）";symbol="kCharacter_Dummy";break;
    case 6437:meaning="目标服务器 ID 无效或当前连接不接受该区服";symbol="kCharacter_InvalidServerId";break;
    case 6438:meaning="已达飞升等级上限";symbol="kCharacter_MaxAscensionGrade";break;
    case 6439:meaning="种族无效";symbol="kCharacter_InvalidRace";break;
    }
    std::ostringstream out;out<<"返回码 "<<result<<" / 0x"<<std::uppercase<<std::hex<<std::setw(4)<<std::setfill('0')<<result<<"："<<meaning;
    if(*symbol)out<<"（"<<symbol<<"）";return out.str();
}
Bytes encodeViewCharRequest(uint32_t serverId,uint64_t characterDbid) {
    if(!serverId || serverId>65535)throw std::invalid_argument("目标服务器 ID 必须为 1..65535");
    if(!characterDbid || characterDbid>0x7fffffffffffffffULL)throw std::invalid_argument("目标角色数据库 ID 必须为正的 64 位有符号整数");
    // Native writer 0x92C34D0 writes u16 + u64 after opcode 0x364F.
    // 0x93312E0 finalizes the reserved four-byte header into ULEB128 length 16.
    Bytes b{16,0x4f,0x36,uint8_t(serverId),uint8_t(serverId>>8)};
    for(unsigned i=0;i<8;++i)b.push_back(uint8_t(characterDbid>>(8*i)));
    return b;
}

std::optional<uint16_t> selfServerId(const GameMessage& message){
    if(!message.structureComplete || message.name.find("自身角色出现")==std::string::npos)return {};
    for(const auto& field:message.fields)if(field.name=="服务器 ID"){
        uint32_t value=0;const auto parsed=std::from_chars(field.value.data(),field.value.data()+field.value.size(),value);
        if(parsed.ec==std::errc{} && parsed.ptr==field.value.data()+field.value.size() && value>0 && value<=65535)return uint16_t(value);
        return {};
    }
    return {};
}
Bytes encodeGuildRequest(bool search,uint8_t order,std::string_view name) {
    if(order>1 || (search && (name.empty() || name.size()>256 || name.find('\0')!=std::string_view::npos)))
        throw std::invalid_argument("军团查询参数无效（名称最多 256 UTF-8 字节）");
    auto var=[](Bytes& b,size_t n){do{auto v=uint8_t(n&127);n>>=7;b.push_back(v|(n?128:0));}while(n);};
    Bytes body{uint8_t(search?0x08:0x06),0x8a};
    if(search){var(body,name.size());body.insert(body.end(),name.begin(),name.end());}
    else body.push_back(order);
    Bytes frame;var(frame,body.size()+4);frame.insert(frame.end(),body.begin(),body.end());return frame;
}
GameFrames splitGameFrames(std::span<const uint8_t> bytes,size_t maxFrame) {
    GameFrames out;
    while(out.consumed<bytes.size()) {
        auto b=bytes.subspan(out.consumed);auto v=varint(b,4);
        if(!v.complete){out.status=v.invalid?"无效长度：最多 4 字节":"等待完整长度";break;}
        if(v.value<4 || uint64_t(v.value)-4+v.size<v.size+2) {out.status="无效长度：不足消息头";break;}
        size_t n=size_t(v.value)-4+v.size;
        if(n>maxFrame){out.status="帧超过大小限制";break;}
        if(n>b.size()){out.status="等待正文：还缺 "+std::to_string(n-b.size())+" 字节";break;}
        out.frames.push_back({out.consumed,n,v.size,v.value});out.consumed+=n;
        if(out.frames.size()>=100000){out.status="达到帧数限制";break;}
    }
    if(out.status.empty())out.status="完整";return out;
}
Bytes decompressLz4Block(std::span<const uint8_t> b,size_t expected,size_t limit) {
    // LZ4 block format: token high nibble = literals; low nibble + 4 = match.
    if(expected>limit)throw std::runtime_error("解压输出超过限制");
    Bytes out;out.reserve(expected);size_t pos=0;
    auto length=[&](size_t n){if(n==15){uint8_t c;do{if(pos==b.size())throw std::runtime_error("LZ4 长度截断");c=b[pos++];if(n>limit || c>limit-n)throw std::runtime_error("LZ4 长度越界");n+=c;}while(c==255);}return n;};
    while(pos<b.size()) {
        auto token=b[pos++];size_t literals=length(token>>4);
        if(literals>b.size()-pos || literals>expected-out.size())throw std::runtime_error("LZ4 字面量越界");
        out.insert(out.end(),b.begin()+pos,b.begin()+pos+literals);pos+=literals;
        if(pos==b.size()){if(out.size()!=expected)throw std::runtime_error("LZ4 输出长度不符");return out;}
        if(b.size()-pos<2)throw std::runtime_error("LZ4 引用截断");
        size_t offset=b[pos]|size_t(b[pos+1])<<8;pos+=2;
        if(!offset || offset>out.size())throw std::runtime_error("LZ4 引用越界");
        size_t match=length(token&15)+4;
        if(match>expected-out.size())throw std::runtime_error("LZ4 匹配长度越界");
        for(size_t i=0;i<match;++i)out.push_back(out[out.size()-offset]);
    }
    throw std::runtime_error("LZ4 缺少末尾字面量序列");
}
void CipherSnapshot::transform(std::span<uint8_t> bytes) {
    for(auto& b:bytes){auto old=i++;auto x=table[old];j=uint8_t(j+x);auto y=table[j];table[old]=y;table[j]=x;b^=table[uint8_t(x+y)];}
}
CipherSnapshot loadCipherSnapshot(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);Bytes b(279);
    if(!f.read(reinterpret_cast<char*>(b.data()),b.size()) || f.peek()!=EOF || std::string(b.begin(),b.begin()+5)!=std::string("A2CS\1",5))throw std::runtime_error("无效的会话状态文件");
    CipherSnapshot s;s.i=b[5];s.j=b[6];s.source.port=uint16_t(readInteger(b,7,2,false));s.destination.port=uint16_t(readInteger(b,9,2,false));s.frameSequence=uint32_t(readInteger(b,11,4,false));
    s.source.address[10]=s.source.address[11]=s.destination.address[10]=s.destination.address[11]=255;
    std::copy_n(b.begin()+15,4,s.source.address.begin()+12);std::copy_n(b.begin()+19,4,s.destination.address.begin()+12);std::copy_n(b.begin()+23,256,s.table.begin());
    auto sorted=s.table;std::sort(sorted.begin(),sorted.end());for(size_t i=0;i<256;++i)if(sorted[i]!=i)throw std::runtime_error("无效的会话置换表");return s;
}
Bytes decryptGameStream(std::span<const uint8_t> bytes,uint32_t firstSequence,const Endpoint& source,const Endpoint& destination,CipherSnapshot state) {
    if(source!=state.source || destination!=state.destination)throw std::runtime_error("会话状态的端点与所选流不符");
    int32_t offset=int32_t(state.frameSequence-firstSequence);
    if(offset<0 || size_t(offset)>=bytes.size())throw std::runtime_error("当前流未包含状态锚点；不能跳过未知密钥流字节");
    auto tail=bytes.subspan(size_t(offset));auto split=splitGameFrames(tail);
    if(split.frames.empty())throw std::runtime_error("状态锚点处没有完整帧");
    Bytes result(tail.begin(),tail.begin()+split.consumed);
    for(auto& frame:split.frames)state.transform(std::span(result).subspan(frame.offset+frame.prefixBytes,frame.length-frame.prefixBytes));
    return result;
}
GameMessage decodeGameFrame(std::span<const uint8_t> bytes,bool inboundPlaintext,bool outboundPlaintext,GameProfile profile) {
    GameMessage m;auto split=splitGameFrames(bytes);
    if(split.frames.size()!=1 || split.consumed!=bytes.size()){m.status="需要一个完整帧："+split.status;return m;}
    const auto& f=split.frames[0];Reader r{bytes,m};r.pos=f.prefixBytes;
    r.add(0,f.prefixBytes,"编码长度","ULEB128",std::to_string(f.encodedLength)+" -> 总长 "+std::to_string(f.length),"IDA 0x9204C90 / 0x9330B00",true);
    if(!inboundPlaintext){m.name="出站 / 未确认正文";m.parsedBytes=r.pos;r.add(r.pos,bytes.size()-r.pos,"原始正文","bytes",hex(bytes.subspan(r.pos).first(std::min<size_t>(32,bytes.size()-r.pos))),"未导入对应会话状态；正文保持原始字节",false);m.status="仅识别长度；正文待分析";return m;}
    const auto opcode=r.u("消息编号",2,"IDA 0x9330B00；x64dbg header-verified");
    std::ostringstream label;label<<"0x"<<std::hex<<std::uppercase<<opcode;m.name=label.str();
    bool supported=true,stop=false;
    try {
        if(outboundPlaintext) {
            if(profile==GameProfile::World && opcode==0x3601){m.name+=" 时间回显";r.u("回显服务器毫秒时间",8,"变换前后 x64dbg / WinDivert 一致性验证");}
            else if(profile==GameProfile::World && opcode==0x3602){
                const std::string ev="IDA 0x9329A40；0x93314D0 写 ULEB128；0x932B710 将响应时刻减去回显值后存入连接 +0xFA0";
                m.name+=" 客户端时间同步请求";
                r.u("客户端日历毫秒时间（时基待确认）",8,ev+"；0x3FA8FF0 生成日历 tick 后除以 10000",false);
                r.var("上次往返延迟（毫秒）",ev);
            }
            else if(profile==GameProfile::World && (opcode==0x8A06 || opcode==0x8A08)){
                m.name+=opcode==0x8A06?" 获取军团列表请求":" 搜索军团请求";
                if(opcode==0x8A06)r.u("排序（0 人数 / 1 排名）",1,"2026-09-30 SendGuildRecommendedList 0x148FF2C90");
                else r.stringBytes("军团名称","2026-09-30 SendGuildSearchList 0x148FF31E0",true);
            }
            else if(profile==GameProfile::World && opcode==0x364F){
                const std::string ev="SendViewChar 0x9090950 → 0x92C34D0；ViewChar_RQ 0xEDF92D0；线上宽度按写入函数核对";
                m.name+=" 查看角色资料请求";
                r.u("目标服务器 ID",2,ev+"；_target_server_id，反射 u32，线上 u16");
                r.u("目标角色数据库 ID",8,ev+"；_target_dbid，不是场景实体编号");
            }
            else if(profile==GameProfile::World && opcode>=0x3700 && opcode<=0x3703){
                const bool gravity=opcode>=0x3702, initial=opcode==0x3702;
                const std::string ev="IDA 0x85B27F0 / 0x85B2A90 → 0x92C5860；0x85B2CF0 → 0x92C5B00；0x85B3070 → 0x92C5EB0；反射 0xEDF95E0..0xEDF9688";
                m.name+=opcode==0x3700?" 自身开始移动上报":opcode==0x3701?" 自身移动更新上报":initial?" 自身重力移动开始上报":" 自身重力移动更新上报";
                auto flags=r.u("可选字段位图",1,ev);r.flagsKnown(flags,initial?31:3);
                if(initial && (flags&1))r.u("重力模式（枚举待确认）",1,ev+"；_gravity_mode",false);
                for(auto axis:{"X","Y","Z"})r.f32(axis,ev+"；_pos / Vec3，单位待确认");
                if(!gravity)r.u("移动方向（编码待确认）",2,ev+"；_move_dir，线上 u16",false);
                r.f32("朝向（单位待确认）",ev+"；_rotate_dir",false);
                if(gravity)for(auto axis:{"X","Y","Z"})r.f32(std::string("移动速度 ")+axis,ev+"；_move_velocity_3d，单位待确认");
                if(flags&(initial?2:1)){stop=true;m.status="自身移动的局部坐标结构尚未解析";}
                else {
                    if(initial && (flags&4))r.boolean("是否向上跳跃",ev+"；_is_jump_up",true);
                    if(initial && (flags&8))r.u("跳板 ID",8,ev+"；_springboard_id / 0x876A820");
                    if(flags&(initial?16:2))r.boolean("启用航位推算",ev+"；_dead_reckoning_on",true);
                    r.u("客户端时间（时基待确认）",8,ev+"；_client_time",false);
                }
            }
            else if(profile==GameProfile::World && opcode==0xFFA1){
                const std::string ev="IDA 0x9093CF0 SendClientCameraView → 0x8585280 → 0x85582E0；0x968DC10 → 0x3F4EFE0 四元数转欧拉角，弧度乘 57.29577951308232";
                m.name+=" 客户端相机视角";
                r.f32("相机欧拉角 C（度；输入分量 2）",ev);
                r.f32("相机欧拉角 A（度；输入分量 0）",ev);
                r.f32("相机欧拉角 B（度；输入分量 1）",ev);
                r.f32("相机缩放 (_zoom；物理单位未确认)",ev+"；0x968D610 读取对象 +0x2D0；ClientCameraViewInfo 属性 0xDC6DF78 对应 +0x20");
                r.var("距上次鼠标输入时间（毫秒）",ev+"；属性 0xDC6DFB0 为 _last_mouse_input_time_ms (+0x24)；0x8F910B0..0x8F91108 以当前计时减对象 +0x128，缩放并钳位后发送");
            }
            else {supported=false;m.status="出站明文已还原，正文结构待确认";}
        } else if(profile==GameProfile::Login) {
            supported=loginFields(r,opcode);if(!supported)m.status="登录服务正文结构尚未确认";
        } else {
        switch(opcode) {
        case 0xE200: {
            const std::string ev="IDA 0x929DAC0 → 0x93166C0 → 0x909B5A0；MessageCardInitData_NT 0xEE09F38";
            m.name+=" 留言卡初始化";auto n=r.count("留言卡数量",ev,36);
            static constexpr const char* types[]={"None","Character","Guild","Max"};
            for(uint32_t i=0;i<n;++i){auto p="留言卡["+std::to_string(i)+"]";
                const std::string meta=ev+"；Common_MessageCard 0xEE20F60 / 属性表 0xDC753A0";
                r.u(p+" 数据库编号 (_message_card_dbid)",8,meta);
                auto type=r.u(p+" 类型 EMessageCardType",1,meta+"；枚举表 0xDD5C310",false);
                if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
                r.stringBytes(p+" 发送者昵称 (_sender_nickname)",meta,true);r.u(p+" 发送者角色编号 (_sender_char_dbid)",8,meta);
                r.stringBytes(p+" 标题 (_title)",meta,true);r.stringBytes(p+" 正文 (_body)",meta,true);
                r.boolean(p+" 已读 (_read)",meta+"；SetBit 0x4262A90 → +0x50",true);
                r.boolean(p+" 已锁定 (_locked)",meta+"；SetBit 0x4C34DF0 → +0x51；位游标跨卡片延续",true);
                r.u(p+" 发送时间（Unix 毫秒原值）",8,meta+"；_sent_time，0x9099600 乘 10000 加日历基准；0 未设置");
                r.u(p+" 到期时间（Unix 毫秒原值）",8,meta+"；_expire_time，同样转换；0 未设置");
            }
            r.u("下次发送计数重置时间（Unix 毫秒原值）",8,ev+"；_next_sent_count_reset_time，消费者以完整 64 位乘 10000 加日历基准；保留负值/哨兵的原始位");
            r.u("角色留言卡已发送次数",1,ev+"；_character_message_card_sent_count");r.u("公会留言卡已发送次数",1,ev+"；_guild_message_card_sent_count");
            auto blocked=r.count("屏蔽记录数量",ev,10);
            for(uint32_t i=0;i<blocked;++i){auto p="屏蔽["+std::to_string(i)+"]";auto meta=ev+"；MessageCardBlock 0xEE20F98 / 属性表 0xDC75318";
                r.stringBytes(p+" 游戏账号标识 (_block_game_account_id)",meta,true);r.u(p+" 角色编号 (_block_char_dbid)",8,meta);r.stringBytes(p+" 昵称 (_block_nickname)",meta,true);
            }break;
        }
        case 0xE324: {
            const std::string ev="IDA 0x92A6210 → 0x93219C0 → 0x8581570 → 0x90A8380；BattlePassInfoList_NT 0xEE0C890 / BattlePassGroupInfo 0xEE21B30";
            m.name+=" Battle Pass 分组信息";auto n=r.count("分组数量",ev,35);
            for(uint32_t i=0;i<n;++i){auto p="分组["+std::to_string(i)+"]";
                r.u(p+" 编号 (_id)",4,ev);r.u(p+" 总经验 (_total_exp)",4,ev);r.u(p+" 周期经验 (_period_exp)",4,ev);
                r.u(p+" 经验重置时间（Unix 毫秒原值）",8,ev+"；_exp_reset_time +0x18，消费者乘 10000 加日历基准；0 未设置");
                r.u(p+" 重复奖励时间（Unix 毫秒原值）",8,ev+"；_repeat_reward_time +0x20，同样转换；0 未设置");
                auto passes=r.count(p+" Battle Pass 列表数量",ev,4);
                for(uint32_t j=0;j<passes;++j)r.u(p+" Battle Pass["+std::to_string(j)+"] 编号",4,ev+"；_battle_pass_list +0x28");
                auto levels=r.count(p+" 已领奖等级映射数量",ev,5);
                for(uint32_t j=0;j<levels;++j){auto entry=p+" 已领奖等级["+std::to_string(j)+"]";
                    r.u(entry+" 键（角色待确认）",4,ev+"；_rewarded_level_list +0x38",false);
                    r.u(entry+" 等级原值（具体对应待确认）",1,ev+"；固定 u8，不是打包 bool",false);
                }
                auto repeats=r.count(p+" 重复奖励经验映射数量",ev,8);
                for(uint32_t j=0;j<repeats;++j){auto entry=p+" 重复奖励["+std::to_string(j)+"]";
                    r.u(entry+" 键（角色待确认）",4,ev+"；_repeat_reward_exp_list +0x88",false);r.u(entry+" 经验值",4,ev+"；_repeat_reward_exp_list 的 u32 值");
                }
                r.boolean(p+" 待处理奖励已领取 (_pending_reward_claimed)",ev+"；SetBit 0x5CBCE60 → +0xD8",true);
                r.u(p+" 已订阅角色数量 (_subscribed_character_count)",4,ev+"；+0xDC");
                r.boolean(p+" 已订阅 (_is_subscribed)",ev+"；SetBit 0x631A3D0 → +0xE0，位游标跨分组和计数共享",true);
            }break;
        }
        case 0xE223:
            m.name+=" 固定双字段通知（用途待确认）";
            r.u("64 位原值（语义待确认）",8,"IDA 0x929F860 先读固定 8 字节，再读 4 字节；此处理器没有业务消费者",false);
            r.u("32 位原值（语义待确认）",4,"IDA 0x929F860；不根据数值外形推断时间或计数",false);break;
        case 0xE25C: {
            const std::string ev="IDA 0x92A1B20 → 0x9317E70 → 0x909E200；ContentsFeatureOnOffListByAdmin_NT 0xEE0B358；事件 System_ContentsFeatureAdmin";
            m.name+=" 管理员玩法功能开关";auto n=r.count("功能数量",ev,1);
            static constexpr const char* types[]={"None","BossChallenge","BossChallenge_UI","DreamCrossRoad","Abyss","Abyss_UI","Daevanion","Conversion","Wing","Ride","Title","PeriodCollection","Guild","Guild_UI","Skin","BMShop","Journal_UI","Skill_UI","PartyDungeon","PartyDungeon_UI","PartyDungeonChallenge","PartyDungeonChallenge_UI","Achievement","Craft","Gather","Post","Enchant","Ranking_UI","Trade","SeasonContentsInfo_UI","SeasonMission","Arcana","Arena","Arena_UI","DailyDungeon","DailyDungeon_UI","BattlePass","StatInfo_UI","ContentsNavigator_UI","Exchange","FieldEvent","SealDungeon","Party_Force","Duel_Challenge","Friend_Request","AutoBattle","AutoBattle_Setting","ShugoFesta","ShugoFesta_UI","Suppression","Suppression_UI","GrowthDungeon","Raid","Raid_UI","AwakenDungeon","AwakenDungeon_UI","PowerSavingMode_UI","Customizing","Attendance","Chat","Snapshot","Pantheon","AutoSkill_UI","AutoSkill_Setting","Map_UI","ItemDelete","ItemDecompose","Rift","RemoteStorage","Preset","HudEdit_UI","DamageAnalyzer","Onboarding","Craft_UI","Pantheon_UI","ItemRestore","ChangeCharacter_UI","RiftEvent","Max"};
            for(uint32_t i=0;i<n;++i){auto p="功能["+std::to_string(i)+"]";
                auto type=r.u(p+" 类型 EContentsFeatureType",1,ev+"；0x8808E30 → 0xEE2F000 → 0xDD49DA0",false);
                if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
                r.boolean(p+" 开关标志（启用/禁用极性待确认）",ev+"；true 从玩家 +0x1E0 对象 +0x78 集合移除，false 加入；不复用 EContentsControlType");
            }break;
        }
        case 0xE262: {
            const std::string ev="IDA 0x92A1E20 → 0x9319B50 → 0x90A2940；TrustLimitInfo_NT 0xEE0B4A8 / Common_TrustLimitInfo 0xEE22268";
            m.name+=" 信任额度信息";
            r.boolean("使用每日信任额度 (_trust_daily_limit_use)",ev+"；首个 bool → 消息 +2 / 玩家 +0x968C",true);
            auto n=r.count("额度条目数量",ev,26);
            static constexpr const char* types[]={"None","Battle","ExchangeKinaLimit","TradeBuyLimit","TradePayoutLimit","ExchangePayoutLimit","Max"};
            for(uint32_t i=0;i<n;++i){auto p="额度["+std::to_string(i)+"]";
                for(auto suffix:{" 映射键"," 记录类型 (_type)"}){auto type=r.u(p+suffix+" EContentsKinaLimitType",1,ev+"；枚举表 0xDD46C70",false);
                    if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}}
                r.u(p+" 使用量 (_usage)",8,ev+"；值对象 +0x10");r.u(p+" 上限原值 (_limit_count)",8,ev+"；值对象 +0x18，特殊哨兵值保留原样");
                r.u(p+" 重置时间（Unix 毫秒；0 未设置）",8,ev+"；_reset_time +0x20；消费者对类型 2..5 乘 10000 加日历基准");
            }
            r.boolean("商城购买允许 (_bmshop_purchase_enable)",ev+"；与首个 bool 共享字节，使用 bit 1，尾部仍必读",true);
            r.u("更新时间原值 (_update_time；单位待确认)",8,ev+"；消息 +0x60，此消费者不使用",false);
            r.u("信任阶段 (_trust_step)",4,ev+"；消息 +0x68 → 玩家 +0x9690");break;
        }
        case 0xE30B: {
            const std::string ev="IDA 0x92A4F00 → 0x92C5770 → 0x90A6BA0；MonolithList_NT 0xEE0C318 → 0x8799A10 → MonolithStatus 0xEE1F4E8";
            m.name+=" Monolith 共鸣状态列表";auto n=r.count("Monolith 数量",ev,8);
            for(uint32_t i=0;i<n;++i){auto p="Monolith["+std::to_string(i)+"]";
                r.u(p+" 配置编号 (_monolith_id)",4,ev+"；0x894D9A0 初始化虚表 0xDE80438，名称函数 0x890BFA0 返回 Monolith");
                r.u(p+" 共鸣次数 (_resonate_count)",4,ev+"；属性表 0xDC81820；消费者另用 MonolithLevel 表计算等级，不将线上次数误标为等级");
            }break;
        }
        case 0xE224: {
            const std::string ev="IDA 0x929F8C0 → 0x9317E70 → 0x909E030；ContentsControl_NT 0xEE0A718；事件 ContentsControl_Update";
            m.name+=" 玩法开关列表";auto n=r.count("玩法开关数量",ev,1);
            static constexpr const char* types[]={"None","Exchange","DreamCrossRoad","Max"};
            for(uint32_t i=0;i<n;++i){auto p="开关["+std::to_string(i)+"]";
                auto type=r.u(p+" 类型 EContentsControlType",1,ev+"；枚举表 0xDD544E0",false);
                if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
                r.boolean(p+" 控制标志（true 对应启用或禁用尚未确认）",ev+"；映射 bool 值，位游标跨条目共享");
            }break;
        }
        case 0xE256: {
            const std::string ev="IDA 0x92A14A0 → 0x9319060 → 0x909E6F0；PeriodCollection 0xEE21708 / PeriodCollectionDelivery 0xEE216D0";
            m.name+=" 限时收藏列表";auto n=r.count("收藏数量",ev,22);
            static constexpr const char* tabs[]={"None","Tab_01","Tab_02","Tab_03","Tab_04","Tab_05","Tab_06","Max"};
            for(uint32_t i=0;i<n;++i){auto p="收藏["+std::to_string(i)+"]";
                r.u(p+" 收藏编号 (_collection_id)",4,ev);
                auto tab=r.u(p+" 分页 EPeriodCollectionTabType",1,ev+"；枚举表 0xDD52A60",false);
                if(tab<std::size(tabs)){r.out.fields.back().value+=" / "+std::string(tabs[tab]);r.out.fields.back().meaningKnown=true;}
                r.u(p+" 开始时间（Unix 毫秒；0 未设置）",8,ev+"；_start_time，消费者乘 10000 加 621355968000000000");
                r.u(p+" 结束时间（Unix 毫秒；0 未设置）",8,ev+"；_end_time，同样转换");
                auto count=r.count(p+" 登记条目数量",ev,12);
                for(uint32_t j=0;j<count;++j){auto entry=p+" 登记["+std::to_string(j)+"]";
                    r.u(entry+" 收藏编号 (_collection_id)",4,ev);r.u(entry+" 列表编号 (_list_id)",4,ev);r.u(entry+" 已登记数量 (_registered_count)",4,ev);
                    r.boolean(entry+" 已领取奖励 (_rewarded)",ev+"；属性 0xDC72140 的 SetBit 函数 0x4262DD0 写 +0x14；跨内外列表共享位游标",true);
                }
            }break;
        }
        case 0xE257: {
            const std::string ev="IDA 0x92A1560；ContentsUnlockList_NT 0xEE0B240，属性 0xDBB9DC0 → 0x8808CE0 → 0xEE2EFD0；事件 System_UnlockContentLoaded";
            m.name+=" 已解锁玩法列表";auto n=r.count("已解锁玩法数量",ev,1);
            static constexpr const char* types[]={"None","Journal","Skill","BMShop","PartyDungeon","DailyDungeon","Arcana","Arena","Achievement","Guild","Craft","Gather","Wing","Conversion","Title","Skin","Ride","Post","DreamCrossRoad","Enchant","Daevanion","Abyss","PeriodCollection","Emblem","BossChallenge","Collection","Ranking","Trade","SeasonContentsInfo","SeasonMission","BattlePass","PartyDungeonChallenge","StatInfo","ContentsNavigator","WorldLevelScale","AutoBattle","ShugoFesta","Suppression","DailyDungeonLobby","GrowthDungeon","Raid","AwakenDungeon","Community","Customizing","Attendance","Chat","Snapshot","PartyDungeon1Tier","PartyDungeon2Tier","PartyDungeon3Tier","Party_Force","Pantheon","Map","ItemDelete","ItemDecompose","Coupon","RemoteStorage","PartyDungeon4Tier","PartyDungeon5Tier","PartyDungeon6Tier","Preset","HudEdit","DamageAnalyzer","Onboarding","ChangeCharacter","Max"};
            for(uint32_t i=0;i<n;++i){auto type=r.u("已解锁["+std::to_string(i)+"] EContentsUnlockType",1,ev+"；枚举表 0xDD4A290",false);
                if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
            }break;
        }
        case 0xE32A: {
            const std::string ev="IDA 0x92A6920 / 0x9322280 / 0x907DE80；ArenaRankingInfo 属性表 0xDC82CF0";
            m.name+=" 竞技场个人排名";auto n=r.count("排名条目数量",ev,14);
            static constexpr const char* types[]={"None","Single","Team","Strategy","Suppression","Max"};
            for(uint32_t i=0;i<n;++i){auto p="排名["+std::to_string(i)+"]";
                auto type=r.u(p+" 匹配类型 EMatchingType",1,ev+"；0x880B5C0 → 0xEE2F5D0 → 0xDD48990",false);
                if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
                r.u(p+" 排名积分 (_ranking_point)",8,ev);
                for(auto fieldLabel:{"胜场 (_win_count)","负场 (_lose_count)","平局 (_draw_count)","退出次数 (_quit_game_count)","连胜次数 (_serial_win_count)"})r.var(p+" "+fieldLabel,ev);
            }break;
        }
        case 0xE33B: {
            const std::string ev="IDA 0x92A75F0 / 0x93230F0 / 0x90715B0；SubscribeInfo 属性表 0xDC70110";
            m.name+=" 订阅列表";auto n=r.count("订阅数量",ev,21);
            static constexpr std::pair<unsigned,const char*> types[]={{0,"None"},{1,"RemoteStorage"},{2,"Exchange"},{3,"TradeExPanded"},{4,"OdenergyCube"},{25,"Shop"},{26,"OdenergyCharge"},{27,"TradeExchangeOpen"},{28,"FieldEventRankCount"},{29,"DailyDungeonUse"},{30,"AbyssUse"},{32,"ChatRace"},{33,"Gather"},{34,"FastQueue"},{35,"RemoteStorage2"},{36,"Exchange2"},{37,"TradeExPanded2"},{38,"TradeExchangeOpen2"},{39,"Shop2"},{40,"OdenergyCube2"},{41,"OdenergyCharge2"},{42,"FieldEventRankCount2"},{43,"AbyssUse2"},{44,"ItemAutoLooting"},{45,"Max"}};
            for(uint32_t i=0;i<n;++i){auto p="订阅["+std::to_string(i)+"]";
                auto type=r.u(p+" 类型 ESubscribeType",1,ev+"；枚举表 0xDD49210，数值非连续",false);
                for(const auto& [value,name]:types)if(type==value){r.out.fields.back().value+=" / "+std::string(name);r.out.fields.back().meaningKnown=true;break;}
                r.u(p+" 数据编号 (_data_id)",4,ev);
                r.u(p+" 开始时间（Unix 毫秒；0 未设置）",8,ev+"；_start_time，消费者乘 10000 加日历基准");
                r.u(p+" 结束时间（Unix 毫秒；0 未设置）",8,ev+"；_end_time，同样转换");
            }break;
        }
        case 0xE33D:
            m.name+=" 个人交易使用次数";r.u("使用次数 (_use_count)",4,"IDA 0x92A7770；PersonalExchangeInfo_NT 属性 0xDBB2C68；事件 Exchange_UseCountChanged");break;
        case 0xE33E: {
            const std::string ev="IDA 0x92A7810 / 0x9323310 / 0x90CE8D0；RankingScoreContentsInfo 0xEE21CB8 / ContentsSeasonKey 0xEE215B8";
            m.name+=" 玩法赛季排名积分";auto n=r.count("赛季条目数量",ev,17);
            static constexpr const char* types[]={"None","Abyss","SeasonMission","BossChallenge","PartyChallenge","ArenaSingle","ArenaTeam","ArenaStrategy","BattlePass","Conqueror","BattlePass1","BattlePass2","BattlePass3","BattlePass4","BattlePass5","BattlePass6","BattlePass7","BattlePass8","BattlePass9","BattlePass10","Suppression","Awaken","Attendance1","Attendance2","Attendance3","Attendance4","Attendance5","ItemSuccession","Onboarding","Onboarding1","Onboarding2","Onboarding3","Onboarding4","Onboarding5","Onboarding6","Onboarding7","Onboarding8","Onboarding9","Onboarding10","EventMission1","EventMission2","EventMission3","EventMission4","EventMission5","EventMission6","EventMission7","EventMission8","EventMission9","EventMission10","Max"};
            for(uint32_t i=0;i<n;++i){auto p="赛季["+std::to_string(i)+"]";
                auto type=r.u(p+" 玩法类型 ESeasonContentsType",1,ev+"；枚举表 0xDD510B0",false);
                if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
                r.u(p+" SeasonScheduleGroup 配置编号",4,ev+"；_season_group_id，消费者查 0x90AD140");
                r.u(p+" 赛季序号 (_season_no)",4,ev);r.u(p+" 排名积分 (_ranking_point)",8,ev);
            }break;
        }
        case 0xE342: {
            const std::string ev="IDA 0x92A7A10 / 0x93235B0；RestorableItemInfo 属性表 0xDC6ECA0";
            m.name+=" 可恢复物品列表";auto n=r.count("物品数量",ev,39);
            static constexpr const char* types[]={"None","NpcShopSell","Extraction","AttuneSoulBind","Max"};
            for(uint32_t i=0;i<n;++i){auto p="物品["+std::to_string(i)+"]";
                r.u(p+" 物品键 (_item_key)",8,ev);r.u(p+" 物品编号 (_item_id)",4,ev);r.u(p+" 数量 (_item_count)",8,ev);
                auto type=r.u(p+" 恢复类型 EItemRestoreType",1,ev+"；0x88106C0 → 0xEE301A0 → 0xDD476B0",false);
                if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
                r.u(p+" 删除时间（Unix 毫秒；0 未设置）",8,ev+"；_deleted_time，0x9063CD0 消费者转换");
                r.u(p+" 到期时间原值 (_expire_time；单位待确认)",8,ev,false);
                for(auto fieldLabel:{"所需物品 (_cost_item_list)","额外物品 (_add_item_list)"}){
                    auto k=r.count(p+" "+fieldLabel+" 数量",ev,12);
                    for(uint32_t j=0;j<k;++j){auto c=p+" "+fieldLabel+"["+std::to_string(j)+"]";r.u(c+" 物品编号",4,ev);r.u(c+" 数量",8,ev);}
                }
            }break;
        }
        case 0xE343: {
            const std::string ev="IDA 0x92A7B60 / 0x9323B80 / 0x90640D0；ItemRestoreStatus 属性表 0xDC6EFD0";
            m.name+=" 物品恢复状态";auto n=r.count("恢复状态数量",ev,21);
            static constexpr const char* types[]={"None","NpcShopSell","Extraction","AttuneSoulBind","Max"};
            for(uint32_t i=0;i<n;++i){auto p="恢复状态["+std::to_string(i)+"]";
                r.u(p+" 角色数据库编号 (_char_dbid)",8,ev);
                auto type=r.u(p+" 恢复类型 EItemRestoreType",1,ev+"；枚举表 0xDD476B0",false);
                if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
                r.u(p+" 恢复次数 (_restore_count)",4,ev);r.u(p+" 上次恢复时间（Unix 毫秒；0 未设置）",8,ev+"；_last_restore_time，消费者乘 10000 加日历基准");
            }break;
        }
        case 0x8D8C: {
            const std::string ev="IDA 0x928F1F0 / 0x92C5770 / 0x90770F0；重建玩家 +0x15C0 列表";
            m.name+=" 双值记录列表 B（用途待确认）";auto n=r.count("记录数量",ev,8);
            for(uint32_t i=0;i<n;++i){auto p="记录["+std::to_string(i)+"]";r.raw(p,8,4,ev);r.raw(p,12,4,ev);}break;
        }
        case 0x5712: {
            const std::string ev="IDA 0x927B8B0 / 0x92E5B70 / 0x8D234F0；事件 0x304 OdEnergyCube_CeilingInfoChanged";
            m.name+=" 奥德能量方块上限信息";auto n=r.count("副本数量",ev,6);
            for(uint32_t i=0;i<n;++i){auto p="副本["+std::to_string(i)+"]";
                r.u(p+" Dungeon 配置编号",4,ev+"；0x88F0CA0 → 0x88CD4D0");
                r.raw(p,12,1,ev);r.raw(p,13,1,ev);
            }break;
        }
        case 0x8D09: {
            const std::string ev="IDA 0x9289740 / 0x92FC240 / 0x90E7860；Quest_Loaded";
            const std::string meta="反射 DevPacketData_Common_Quest：0xEE1F440 / 属性表 0xDC81260；0x41C12A0 从描述 +0x32 读取成员偏移";
            m.name+=" 任务列表";auto n=r.count("任务数量",ev,22);
            static constexpr const char* statuses[]={"None","Acquirable","Incomplete","Complete","Rewarded","Max"};
            for(uint32_t i=0;i<n;++i){auto p="任务["+std::to_string(i)+"]";
                r.u(p+" Quest 配置编号 (_quest_id)",4,meta+"；+8 经 0x8FF3420 / 0x8B99600 查 Quest 表 0x8948CD0 / 0x890BFF0");
                auto status=r.u(p+" 状态 EQuestStatus",1,meta+"；+0xC，0x87F6FB0 → 0xEE2C630 → 0xDD5B3B0",false);
                if(status<std::size(statuses)){r.out.fields.back().value+=" / "+std::string(statuses[status]);r.out.fields.back().meaningKnown=true;}
                r.var(p+" 步骤序号 (_step_order)",meta+"；+0x10，客户端使用时减 1");
                for(int j=1;j<=6;++j)r.var(p+" 目标值 "+std::to_string(j)+" (_goal"+std::to_string(j)+")",meta+"；+0x14..+0x28");
                r.u(p+" 任务 UID (_uid)",4,meta+"；+0x2C，用于任务实例映射");
                r.var(p+" _roll_count（计数用途待确认）",meta+"；+0x30",false);
                r.u(p+" 发布者编号 (_publisher_id)",4,meta+"；+0x34");
                auto k=r.count(p+" 随机奖励物品数量",ev+"；反射 +0x38 _random_reward_items_with_prob",14);
                for(uint32_t j=0;j<k;++j){auto c=p+".奖励["+std::to_string(j)+"]";
                    const std::string re="反射 RewardItemWithProb：0xEE1F408 / 属性表 0xDC81150；读取器 0x92FC240";
                    r.u(c+" Item 配置编号 (_item_id)",4,re);r.var(c+" 数量 (_count)",re);
                    r.u(c+" 强化等级 (_enchant_level)",1,re);r.u(c+" 概率原值 (_prob；比例待确认)",8,re,false);
                }
            }break;
        }
        case 0x8D7E: {
            const std::string ev="IDA 0x928E960 / 0x92C5770 / 0x90753E0";m.name+=" 双值记录列表（用途待确认）";
            auto n=r.count("记录数量",ev,8);
            for(uint32_t i=0;i<n;++i){auto p="记录["+std::to_string(i)+"]";r.raw(p,8,4,ev);r.raw(p,12,4,ev);}break;
        }
        case 0x8D37: {
            const std::string ev="IDA 0x928B180 / 0x92FE9A0 / 0x9073460";m.name+=" 采集技能列表";
            auto n=r.count("技能数量",ev,8);
            for(uint32_t i=0;i<n;++i){auto p="技能["+std::to_string(i)+"]";
                r.u(p+" GatherSkill 配置编号",4,"IDA 0x8D819F0 -> 0x8AF3020 / 0x8AB9A70");
                r.u(p+" 数值（语义待确认）",4,"IDA 0x8D819F0 -> 玩家 +0x14F8 / 0x8F9EC20",false);
            }
            r.u("技能点 A（角色待确认）",2,"IDA 0x9073460 -> 玩家 +0x14F0 / Gather_SkillPointUpdate",false);
            r.u("技能点 B（角色待确认）",2,"IDA 0x9073460 -> 玩家 +0x14F2 / Gather_SkillPointUpdate",false);break;
        }
        case 0x8D1C: {
            const std::string ev="IDA 0x928A230 / 0x92FCFD0 / 0x90E9C00";m.name+=" DutyMission 任务记录";
            r.raw("消息",4,4,ev);r.raw("消息",8,4,ev);
            auto n=r.count("任务组数量",ev,18);
            for(uint32_t i=0;i<n;++i){auto p="任务组["+std::to_string(i)+"]";
                const std::string dm="反射 DevPacketData_Common_DutyMissionQuest：0xEE1F478 / 0xDC81600；成员偏移由 0x41C12A0 读取";
                r.u(p+" 记录 UID (_uid)",4,dm);r.u(p+" Quest 配置编号 (_quest_id)",4,dm);
                r.u(p+" 任务 UID (_quest_uid)",4,dm);r.u(p+" 发布者编号 (_publisher_id)",4,dm);
                static constexpr const char* statuses[]={"None","Acquirable","Incomplete","Complete","Rewarded","Max"};
                auto status=r.u(p+" 状态 EQuestStatus",1,dm+"；0x87F6FB0 → 枚举表 0xDD5B3B0",false);
                if(status<std::size(statuses)){r.out.fields.back().value+=" / "+std::string(statuses[status]);r.out.fields.back().meaningKnown=true;}
                auto k=r.count(p+" 随机奖励子项数量",ev,14);
                for(uint32_t j=0;j<k;++j){auto c=p+".子项["+std::to_string(j)+"]";
                    r.u(c+" Item 配置编号",4,"IDA 0x90E9C00 -> 0xF290A50 / 0x88F6DB0 / 0x88CCBA0");
                    const std::string re="反射 RewardItemWithProb：0xEE1F408 / 属性表 0xDC81150；读取器 0x92FCFD0";
                    r.var(c+" 数量 (_count)",re);r.u(c+" 强化等级 (_enchant_level)",1,re);r.u(c+" 概率原值 (_prob；比例待确认)",8,re,false);
                }
            }
            const std::string tev="IDA 0x90EA181..0x90EA1E3：u64 * 10000 + 621355968000000000；0 未设置";
            r.u("Unix 毫秒时间 A（用途待确认）",8,tev,false);
            auto k=r.count("附加编号数量",ev,4);
            for(uint32_t i=0;i<k;++i)r.u("附加编号["+std::to_string(i)+"]（语义待确认）",4,"IDA 0x8E8E990 -> 0x8E9DE00",false);
            r.u("Unix 毫秒时间 B（用途待确认）",8,tev,false);break;
        }
        case 0x9331: {
            const std::string ev="IDA 0x9296D90 → 0x9312570 → 0x90C4C20；每条固定 50 字节，交玩家副本对象处理";
            m.name+=" 副本赛季关联记录";auto n=r.count("副本赛季记录数量",ev,50);
            for(uint32_t i=0;i<n;++i){const auto p="副本赛季记录 "+std::to_string(i);
                r.raw(p,0x10,1,ev+"；转换器未使用，但线上必读");
                r.u(p+" SeasonScheduleGroup 配置编号",4,ev+"；0x90AD140 初始化虚表 0xDEE87E8 → 0x90309A0 返回 SeasonScheduleGroup");
                for(unsigned offset:{0x18u,0x20u,0x24u})r.raw(p,offset,4,ev);
                r.raw(p,0x28,1,ev);r.raw(p,0x30,8,ev);
                r.raw(p,0x38,4,ev);r.raw(p,0x3c,4,ev);r.raw(p,0x40,8,ev);r.raw(p,0x48,8,ev);
            }break;
        }
        case 0x3657: {
            const std::string ev="IDA 分派 0x926365A..0x926379E；事件 0x31C MentoringRole_Update；反射类型 DevPacketData_GameServer_MentoringRoleUpdate_NT";
            m.name+=" 导师角色更新";
            static constexpr const char* roles[]={"None","Mentor","Mentee","Mentee_Inactive","Max"};
            auto role=r.u("EMentoringRole",1,ev+"；属性 _mentoring_role，0x88120A0 → 0xEE30560 → 枚举表 0xDD46AE0",false);
            if(role<std::size(roles)){r.out.fields.back().value+=" / "+std::string(roles[role]);r.out.fields.back().meaningKnown=true;}
            r.u("角色到期时间（Unix 毫秒；0 表示未设置）",8,ev+"；同一反射类型属性 _expire_time；读取值按 i64 大于零时乘 10000 加 621355968000000000，否则客户端保存 0");
            break;
        }
        case 0x610B: {
            const std::string ev="IDA 0x927F2E0 → 0x92E9460；0x8FA33D0 保存 ContentsTicket 记录，列表路径无增量提示";
            m.name+=" 玩法门票状态列表";
            auto n=r.count("门票记录数量",ev,5);
            for(uint32_t i=0;i<n;++i){const auto p="门票记录 "+std::to_string(i);auto flags=r.u(p+" 可选字段掩码",1,ev);
                r.u(p+" ContentsTicket 配置编号",4,ev+"；0x8A8B0C0 → vtable 0xDE92658 → 0x8A68AC0");
                if(flags&1)r.u(p+" 时间 A（毫秒；用途待确认）",8,ev+"；消费者乘 10000 保存到 +0x10",false);
                if(flags&2)r.u(p+" 时间 B（毫秒；用途待确认）",8,ev+"；消费者乘 10000 保存到 +0x18",false);
                if(flags&4)r.var(p+" 数量 A（用途待确认）",ev+"；记录 +0x20",false);
                if(flags&8)r.var(p+" 数量 B（用途待确认）",ev+"；记录 +0x24",false);
            }break;
        }
        case 0x6103: {
            const std::string ev="IDA 0x927EC40 → 0x92E8C10 → 0x8D1FFE0；保存玩家副本对象 +0x708 映射";
            m.name+=" 副本计时状态列表";
            auto n=r.count("副本记录数量",ev,5);
            for(uint32_t i=0;i<n;++i){const auto p="副本记录 "+std::to_string(i);auto flags=r.u(p+" 可选字段掩码",1,ev);
                r.u(p+" Dungeon 配置编号",4,ev+"；0x88F0CA0 初始化虚表 0xDE80058 → 0x88CD4D0 返回 Dungeon");
                if(flags&1)r.u(p+" 附加字节（含义待确认）",1,ev+"；与 Dungeon +0xC0 引用表数量比较，相等时置记录标志，否则加 1 后保存",false);
                if(flags&2)r.var(p+" 计时值（秒；用途待确认）",ev+"；转 double 乘 10000000 后保存到记录 +0x18",false);
                if(flags&4)r.rawVar(p,0x14,ev+"；保存到记录 +0x10");
            }break;
        }
        case 0x9232: {
            const std::string ev="IDA 0x92956C0 → 0x8D72780 → 0x8D73770；TeleportArtifact_Engraved / Unengraved 事件 0x2EA / 0x2EB";
            m.name+=" 传送神器刻印列表";auto n=r.count("刻印配置数量",ev,4);
            for(uint32_t i=0;i<n;++i)r.u("刻印 "+std::to_string(i)+" TeleportArtifact 配置编号",4,ev+"；0x8937250 初始化虚表 0xDE81658 → 0x890C130 返回 TeleportArtifact");
            break;
        }
        case 0x9330: {
            const std::string ev="IDA 0x9296C30 → 0x9312450；清空玩家 +0x92A0 的各分类内部映射，再按 u8 分类和 u32 键存储 u64 值";
            m.name+=" 分类映射同步（业务含义待确认）";auto n=r.count("分类映射条目数量",ev,13);
            for(uint32_t i=0;i<n;++i){const auto p="分类映射条目 "+std::to_string(i);r.raw(p,8,4,ev);r.raw(p,0xc,1,ev);r.raw(p,0x10,8,ev);}
            break;
        }
        case 0x610C: {
            const std::string ev="IDA 0x927F420 → 0x8FA33D0；x64dbg 0x148578100..0x1485781C3 运行时读取函数；事件 0x2EC ContentsTicket_Update";
            m.name+=" 玩法门票状态更新";
            auto outer=r.u("外层可选字段掩码",1,ev);
            auto flags=r.u("门票可选字段掩码",1,ev);
            r.u("ContentsTicket 配置编号",4,ev+"；0x8A8B0C0 表初始化 → vtable 0xDE92658 → 0x8A68AC0 返回 ContentsTicket");
            if(flags&1)r.u("门票时间 A（毫秒；用途待确认）",8,ev+"；乘 10000 后保存到门票记录 +0x10",false);
            if(flags&2)r.u("门票时间 B（毫秒；用途待确认）",8,ev+"；乘 10000 后保存到门票记录 +0x18",false);
            if(flags&4)r.var("门票数量 A（用途待确认）",ev+"；保存到门票记录 +0x20",false);
            if(flags&8)r.var("门票数量 B（用途待确认）",ev+"；保存到门票记录 +0x24",false);
            r.u("更新类型（1 可触发增量提示；其余枚举待确认）",1,ev,false);
            if(outer&1)r.u("提示增量（时间型为秒；数量型为次数）",4,ev+"；传入 0x8FA33D0 第三个参数：时间提示 0x563 / 数量提示 0x562");
            break;
        }
        case 0x3600:
            m.name+=" 时间 / 帧标记";r.u("服务器 Unix 毫秒时间",8,"IDA 0x9261BCA..0x9261C0B");break;
        case 0x3602:case 0x3603:
            m.name+=" 时间同步";r.u("同步附加值（语义待确认）",2,"IDA 0x932B79D",false);
            r.u("回显客户端时间（毫秒；时基待确认）",8,"IDA 0x932B7B0 / 0x932B857",false);
            r.u("服务器 Unix 毫秒时间",8,"IDA 0x932B7C3 / 0x91DDC20");break;
        case 0x3611: {
            const std::string ev="IDA 0x92B8630 / 0x90DB470；RecvGameServerHandShake";
            m.name+=" 游戏服务器握手响应";r.u("握手结果码（0=成功；其他枚举待确认）",2,ev,false);r.raw("握手对象",4,4,ev);
            auto n=r.var("握手数据字节数",ev);r.need(n);
            r.add(r.pos,n,"握手数据（内部结构待确认）","bytes",hex(r.b.subspan(r.pos,n).first(std::min<size_t>(n,64))),ev+"；经过虚函数转换后传入 0x9204620",false);r.pos+=n;
            r.raw("握手对象",0x18,4,ev);r.raw("握手对象",0x1c,4,ev);
            r.i32("日历时间偏移（小时）",ev+"；0x90DB6D8 符号转换，乘 36000000000 存入 0xF09A438；0x8BE5B57/0x8BE5B7A 加到日历时间后转换",true);break;
        }
        case 0x3615: {
            const std::string ev="IDA 0x92B8B80 / 0x90DBEE0；RecvLogin",p="世界登录对象";
            m.name+=" 游戏服务器登录响应";r.u("登录结果码（0=成功；其他枚举待确认）",2,ev,false);
            r.u("基础同步时间（Unix 毫秒）",8,ev+" → 0x91DD950：乘 10000 后加 621355968000000000，转成从公元 1 年起的 100ns tick");
            r.u("同步时间 B（毫秒；用途待确认）",8,ev+" → 0x91DD950：与基础同步时间的差值乘 10000 存入 0xF09A3E8",false);
            auto n=r.count("登录条目数量",ev,100);for(uint32_t i=0;i<n;++i)worldLoginEntry(r,"登录条目 "+std::to_string(i));
            r.raw(p,0x28,1,ev);r.rawBool(p,0x29,ev);r.rawBool(p,0x2a,ev);
            for(auto name:{"登录字符串 A","登录字符串 B","登录字符串 C"})r.stringBytes(name,ev);
            r.rawBool(p,0x60,ev);r.raw(p,0x61,1,ev);r.rawBool(p,0x62,ev);r.rawVar(p,0x64,ev);r.rawBool(p,0x68,ev);r.raw(p,0x69,1,ev);break;
        }
        case 0x3620: {
            const std::string ev="IDA 0x92B9B00 / 0x926B980；RecvSelectCharacter",p="角色选择响应";
            m.name+=" 角色选择响应";r.u("选择结果码（0=成功；其他枚举待确认）",2,ev,false);
            r.var("选中角色的列表编号",ev+"；0x926BA92 → 0x8FED950 与登录角色表、改名处理共用编号；当前样本 3558 对应登录条目");
            r.raw(p,8,4,ev);r.raw(p,0x10,8,ev);r.rawVar(p,0x18,ev);
            r.blob("角色配置压缩数据",ev);
            {auto& blob=m.fields.back();m.config=decodeCharacterConfig(r.b.subspan(blob.offset,blob.size));
                if(m.config.complete){blob.type="u32 + zlib / UTF-16LE JSON";blob.meaningKnown=true;
                    blob.evidence=ev+"；抓包内容经 zlib 校验、长度匹配及 UTF-16LE JSON 验证；配置字段在独立 JSON 路径表中显示";}}
            auto n=r.count("角色选择附加条目数量",ev,9);
            for(uint32_t i=0;i<n;++i){auto item="选择条目 "+std::to_string(i);r.raw(item,8,4,ev);r.raw(item,0xc,4,ev);r.raw(item,0x10,1,ev);}
            r.blob("角色选择数据 B",ev);r.raw(p,0x50,8,ev);break;
        }
        case 0x3621: {
            const std::string ev="IDA 0x92BA160 / 0x9033890；RecvPrepareEnterMap",p="进入地图准备";
            m.name+=" 准备进入地图";r.raw(p,4,4,ev);
            r.u("目标 Map 配置编号",4,ev+" → 0x8A68620 / 0x88F7CC0；虚表 0xDE7F858，名称 0x88C8BF0 返回 Map");
            r.u(p+" 时序值（毫秒；用途待确认）",8,ev+"；乘 10000 交给 0x8DB24F0 计算周期余数",false);
            for(auto axis:{"X","Y","Z"})r.f32(p+" "+axis,ev+"；+0x20/+0x24/+0x28 转为位置向量");
            r.f32(p+" 方向分量（轴与单位待确认）",ev+"；+0x30 转为旋转向量的第二个 double",false);
            r.raw(p,0x34,1,ev);r.raw(p,0x35,1,ev);
            r.u(p+" 时间（毫秒；用途待确认；0 为空值）",8,ev+"；非零乘 10000 加日历纪元常量",false);
            auto n=r.count(p+" 附加条目数量",ev,13);
            for(uint32_t i=0;i<n;++i){auto q=p+" 条目 "+std::to_string(i);r.raw(q,8,4,ev);r.raw(q,0xc,1,ev);r.raw(q,0x10,8,ev);}
            r.rawVar(p,0x50,ev);r.stringBytes(p+" 字符串",ev);r.raw(p,0x68,1,ev);break;
        }
        case 0x3623: {
            const std::string ev="IDA 0x926BC70 → 0x8FADCA0；GameFlow_RecvLoadComplete（0x398）",p="地图载入响应";
            m.name+=" 地图载入完成响应";r.u("载入结果码（0=成功；其他枚举待确认）",2,ev,false);r.raw(p,4,4,ev);
            for(auto axis:{"X","Y","Z"})r.f32(p+" "+axis,ev+"；0x8FAE30F 日志明确标注 X/Y/Z");
            r.raw(p,0x20,4,ev);
            auto value=r.var(p+" 编码值（用途待确认）",ev+"；异或 0x664 后保存至玩家 +0x967C",false);
            m.fields.back().value+=" / 客户端保存值 "+std::to_string(value^0x664u);break;
        }
        case 0xE22E: {
            const std::string ev="IDA 0x92D48C0 → 0x929FE60 → 0x8CA8B80；Update_Currency（0xCD）";
            m.name+=" Daevanion 点数状态";auto n=r.count("点数状态数量",ev,3);
            static constexpr const char* pointTypes[]={"None","DaevanionCrystal","ConquestCrystal","BattleCrystal","Season02Crystal01","Season03Crystal01","Max"};
            for(uint32_t i=0;i<n;++i){auto p="点数状态 "+std::to_string(i);auto type=r.u(p+" EDaevanionPointType",1,ev+"；0x8CD8650 → 0x880DDB0，反射表 0xDD496E0",false);
                if(type<std::size(pointTypes)){m.fields.back().value+=" / "+std::string(pointTypes[type]);m.fields.back().meaningKnown=true;}
                r.i16(p+" 附加状态值（用途待确认）",ev+"；0x929FF16 movsx；保存点数表记录 +8，与 +4 的当前数量分开",false);}
            break;
        }
        case 0xE226: {
            const std::string ev="IDA 0x9318010 → 0x909D3C0 → 0x8CA9810；Daevanion_Loaded（0x39F）";
            m.name+=" Daevanion 节点分组";auto n=r.count("Daevanion 分组数量",ev,5);
            for(uint32_t i=0;i<n;++i){auto p="Daevanion 分组 "+std::to_string(i);r.u(p+" 分组键（身份待确认）",4,ev+"；保存到对象 +0x78 集合",false);
                auto k=r.count(p+" 节点数量",ev,4);for(uint32_t j=0;j<k;++j)r.u(p+" DaevanionNode 配置编号 "+std::to_string(j),4,ev+"；表 0xF2CB9B0，初始化 0x8CC5570，虚表 0xDEDDDC8，名称函数 0x8CA7F80");}
            break;
        }
        case 0xE237: {
            const std::string ev="IDA 0x93181D0 → 0x90CE140 → 0x8D19360 / 0x8D22D10；BossChallengeDungeon_Update（0x31D）";
            m.name+=" Boss 挑战副本记录";auto n=r.count("Boss 挑战记录数量",ev,53);
            for(uint32_t i=0;i<n;++i){auto p="Boss 挑战记录 "+std::to_string(i);r.raw(p,0x10,1,ev+"；转换消费者未使用");r.raw(p,0x14,4,ev);r.raw(p,0x18,4,ev);
                r.u(p+" BossChallengeGroup 配置编号",4,ev+"；记录键供 0x8D1FD60 查询；0x9D05000 从表 0x895BE70 的配置键查同表，名称函数 0x890BEA0");
                r.raw(p,0x24,4,ev+"；转换消费者未使用");r.raw(p,0x28,4,ev);r.raw(p,0x30,8,ev);r.raw(p,0x38,4,ev);r.raw(p,0x3c,4,ev);
                r.u(p+" 时间（毫秒；用途待确认；0 为空值）",8,ev+"；+0x40 非零乘 10000 加日历纪元常量",false);r.raw(p,0x48,8,ev);}
            break;
        }
        case 0xE28C: {
            const std::string ev="IDA 0x931CAF0 → 0x9064340 → 0x8E87940；预设事件表 0xD784AC0";
            m.name+=" 多系统预设同步";
            for(auto category:{"装备","Arcana"}){
                const std::string p=category;r.u(p+" 当前预设键",1,ev+"；分别触发 UpdateEquipPreset / UpdateArcanaPreset");
                auto n=r.count(p+" 预设数量",ev,2);
                for(uint32_t i=0;i<n;++i){auto q=p+" 预设 "+std::to_string(i);r.u(q+" 键",1,ev);auto k=r.count(q+" 条目数量",ev,9);
                    for(uint32_t j=0;j<k;++j){auto t=q+" 条目 "+std::to_string(j);r.u(t+" 关联编号（身份待确认）",8,ev+"；完整 u64 保存到预设列表",false);r.raw(t,0x10,1,ev+"；消费者转换时未使用该字节");}}
            }
            r.u("技能 当前预设键",1,ev+"；Skill_PresetChanged");auto n=r.count("技能预设条目数量",ev,3);
            for(uint32_t i=0;i<n;++i){auto p="技能预设条目 "+std::to_string(i);auto flags=r.u(p+" 可选位图",1,ev);r.flagsKnown(flags,3);
                r.u(p+" 预设键",1,ev);r.u(p+" 分类（枚举待确认）",1,ev,false);
                if(flags&1)r.i32(p+" 单项值（用途待确认）",ev+"；消费者仅取正值",false);
                if(flags&2){auto k=r.count(p+" 候选数量",ev,4);for(uint32_t j=0;j<k;++j)r.i32(p+" 候选 "+std::to_string(j)+"（用途待确认）",ev+"；消费者仅取正值",false);}
            }
            r.u("称号 当前预设键",1,ev+"；Title_PresetChanged");n=r.count("称号预设条目数量",ev,6);
            for(uint32_t i=0;i<n;++i){auto p="称号预设条目 "+std::to_string(i);r.u(p+" 预设键",1,ev);r.u(p+" 子键（枚举待确认）",1,ev,false);r.raw(p,0xc,4,ev);}
            r.u("翅膀 当前预设键",1,ev+"；UpdateWingPreset");r.raw("预设同步",0x64,4,ev+"；读取器必读，当前消费者未使用");
            n=r.count("翅膀预设条目数量",ev,5);
            for(uint32_t i=0;i<n;++i){auto p="翅膀预设条目 "+std::to_string(i);r.u(p+" 预设键",1,ev);r.raw(p,0xc,4,ev);}
            r.u("CreatureBoard 当前预设键",1,ev+"；UpdateCreatureBoardPreset");r.u("据点装饰 当前预设键",1,ev+"；UpdateAgitDecoPreset");
            n=r.count("据点装饰预设数量",ev,2);
            for(uint32_t i=0;i<n;++i){auto p="据点装饰预设 "+std::to_string(i);r.u(p+" 预设键",1,ev);auto k=r.count(p+" 条目数量",ev,5);
                for(uint32_t j=0;j<k;++j){auto q=p+" 条目 "+std::to_string(j);r.raw(q,8,4,ev);r.u(q+" 子键（枚举待确认）",1,ev,false);}}
            break;
        }
        case 0xE351: {
            const std::string ev="IDA 0x9324F90 → 0x90A9C00 → 0x8F37AC0；MembershipPass_InfoChanged（0x473）";
            m.name+=" 会员通行证状态";auto flags=r.u("通行证可选位图",1,ev);r.flagsKnown(flags,3);
            r.u("MembershipPass 配置编号",4,ev+"；0x895F590 虚表 0xDE810B8，名称函数 0x890BE60");r.raw("通行证",8,1,ev);
            for(auto suffix:{" 时间 A"," 时间 B"})r.u(std::string("通行证")+suffix+"（毫秒；用途待确认；0 为空值）",8,ev+"；非零乘 10000 加日历纪元常量",false);
            r.rawBool("通行证",0x20,ev);
            if(flags&1){auto n=r.count("重置成就数量",ev,4);for(uint32_t i=0;i<n;++i)r.u("重置成就编号 "+std::to_string(i),4,ev+"；0x8BE15A0 清零成就进度、状态和时间");}
            if(flags&2){auto n=r.count("重置成就组数量",ev,4);for(uint32_t i=0;i<n;++i)r.u("重置成就组编号 "+std::to_string(i),4,ev+"；0x8BE1A60 清零组内布尔状态");}
            break;
        }
        case 0x8AAF: {
            const std::string ev="IDA 0x92DF2C0 / 0x92881F0 → 0x8DB1AC0 → 0x9496F90";
            m.name+=" 据点装饰配置映射";auto n=r.count("装饰映射数量",ev,5);
            for(uint32_t i=0;i<n;++i){auto p="装饰映射 "+std::to_string(i);
                r.u(p+" AgitDecoItem 配置编号",4,ev+"；保存实体 +0x588 对象的 +0x114，查询 0x8FC1BE0；名称函数 0x8F896B0");
                r.u(p+" 映射键（枚举待确认）",1,ev+"；按此字节查玩家据点对象 +0x120 表中的实体引用",false);}
            break;
        }
        case 0x8D5B: {
            const std::string ev="IDA 0x9301010 → 0x90839D0；共享 bool 游标跨全部集合延续";
            m.name+=" 玩家多集合状态同步";auto n=r.count("状态表 A 数量",ev,8);
            for(uint32_t i=0;i<n;++i){auto p="状态 A "+std::to_string(i);r.raw(p,8,4,ev);r.raw(p,0x18,2,ev);r.raw(p,0x1a,2,ev);}
            n=r.count("状态表 B 数量",ev,5);
            for(uint32_t i=0;i<n;++i){auto p="状态 B "+std::to_string(i);r.u(p+" 键",1,ev,false);r.u(p+" 值",4,ev,false);}
            n=r.count("状态表 C 数量",ev,4);
            for(uint32_t i=0;i<n;++i){auto p="状态 C "+std::to_string(i);r.u(p+" 键",4,ev,false);r.boolean(p+" 标志",ev);}
            r.raw("多集合状态",0xb8,1,ev);
            n=r.count("状态表 D 数量",ev,2);
            for(uint32_t i=0;i<n;++i){auto p="状态 D "+std::to_string(i);r.u(p+" 外层键",1,ev,false);auto k=r.count(p+" 子项数量",ev+" / 0x8582E10",14);
                for(uint32_t j=0;j<k;++j){auto q=p+" 子项 "+std::to_string(j);r.u(q+" 键",1,ev,false);r.raw(q,8,1,ev);r.raw(q,0x10,8,ev);r.raw(q,0x18,4,ev);}}
            n=r.count("状态表 E 数量",ev,2);
            for(uint32_t i=0;i<n;++i){auto p="状态 E "+std::to_string(i);r.u(p+" 外层键",1,ev,false);auto k=r.count(p+" 子项数量",ev+" / 0x8583300",3);
                for(uint32_t j=0;j<k;++j){auto q=p+" 子项 "+std::to_string(j);r.u(q+" 键",1,ev,false);r.raw(q,8,1,ev);
                    for(auto o:{9,10,11})r.rawBool(q,o,ev+" / 0x8583300");r.rawVar(q,0xc,ev);}}
            break;
        }
        case 0x572D: {
            const std::string ev="IDA 0x92E66E0（原始汇编确认）→ 0x906B230 → 0x8E02F00；SpecialDistribution_Add（0x448）";
            m.name+=" 特殊分配记录更新";auto n=r.count("分配记录数量",ev,33);
            for(uint32_t i=0;i<n;++i){auto p="分配记录 "+std::to_string(i);
                r.u(p+" 记录键（具体身份待确认）",8,ev+"；在消费者 +0x50 列表中用于查重更新",false);
                r.raw(p,0x10,1,ev);r.raw(p,0x14,4,ev);r.raw(p,0x18,4,ev);
                r.u(p+" 时间（毫秒；用途待确认；0 为空值）",8,ev+"；非零乘 10000 加日历纪元常量",false);r.raw(p,0x28,8,ev);}
            break;
        }
        case 0x570F: {
            const std::string ev="IDA 0x92E4E50 → 0x927B4B0 / 0x9096F40；MonsterCube_RewardList（0x397）";
            m.name+=" MonsterCube 奖励列表响应";r.u("奖励列表结果码（0=成功；其他枚举待确认）",2,ev,false);
            auto n=r.count("奖励记录数量",ev,26);
            for(uint32_t i=0;i<n;++i){auto p="奖励记录 "+std::to_string(i);
                r.u(p+" 记录键（具体身份待确认）",8,ev+"；0x9096F40 以 +8 建表",false);r.raw(p,0x10,4,ev);r.raw(p,0x14,1,ev);
                r.u(p+" 时间（毫秒；用途待确认；0 为空值）",8,ev+"；非零乘 10000 加日历纪元常量",false);
                r.raw(p,0x20,4,ev);r.rawBool(p,0x24,ev);r.raw(p,0x25,1,ev);}
            break;
        }
        case 0x8A45: {
            const std::string ev="IDA 0x92845F0 → 0x8DB0310；MyGuild_AgitUpdate（0x1CB）";
            m.name+=" 公会据点相关响应";r.u("据点结果码（0=成功；其他枚举待确认）",2,ev,false);
            r.u("据点关联值（语义待确认）",4,ev+"；保存到玩家公会对象 +0x230",false);
            r.u("据点时间（毫秒；用途待确认；0 为空值）",8,ev+"；非零转日历 tick，保存公会对象 +0x238，参与按日计算",false);break;
        }
        case 0xE349: {
            const std::string ev="IDA 0x9324630 → 0x909AEC0 / 0x9099850；Post_PostListUpdate / RedDot_Post";
            m.name+=" 附加邮件列表响应";r.u("邮件结果码（0=成功；其他枚举待确认）",2,ev,false);
            r.u("邮件列表时间（毫秒；用途待确认；0 为空值）",8,ev+"；非零转日历 tick；有效时间时替换列表",false);
            auto n=r.count("附加邮件数量",ev,37);
            for(uint32_t i=0;i<n;++i){auto p="附加邮件 "+std::to_string(i);r.raw(p,8,1,ev);
                r.i32(p+" 组合键低项（业务含义待确认）",ev+"；0x9099880 movsxd，符号扩展后与下一字节左移 32 位相加",false);
                r.u(p+" 组合键高项（业务含义待确认）",1,ev,false);
                for(auto suffix:{" 字符串 +0x18"," 字符串 +0x28"," 字符串 +0x38"})r.stringBytes(p+suffix,ev);
                for(auto suffix:{" 时间 A"," 时间 B"})r.u(p+suffix+"（毫秒；用途待确认；0 为空值）",8,ev+"；非零乘 10000 加日历纪元常量",false);
                r.u(p+" Item 配置编号",4,ev+"；0x90999F2 以 +0x58 查询 0x88F6C50 的 Item 表");
                r.u(p+" 附加值（完整 u64；业务含义待确认）",8,ev+"；消费者 0x9099C32 仅取低 32 位，原始 64 位仍保留",false);}
            break;
        }
        case 0xE34E: {
            const std::string ev="IDA 0x92A8460 → 0x8C253B0；成功路径创建 DialogInitParam_Attendance（0x87CD980 原始汇编）";
            m.name+=" 签到界面相关响应";r.u("签到结果码（0=成功；其他枚举待确认）",2,ev,false);break;
        }
        case 0x9507: case 0x9508: {
            const bool start=opcode==0x9507;
            const std::string ev=start?"IDA 0x9298950 → 0x941F410":"IDA 0x9298B80 → 0x941F670";
            m.name+=start?" 社交动作开始":" 社交动作停止";
            r.var("场景实体编号",ev+"；类型 1 实体键交给 0x94B0BD0 查询");
            r.u("SocialAction 配置编号",4,ev+"；开始路径 0x934B900，虚表 0xDF07808，名称函数 0x93365D0；停止路径仍读取但不使用",true);
            r.u("动作状态（枚举待确认）",1,ev,false);
            r.u("动作目标实体编号",4,ev+"；开始路径构造类型 1 的实体引用；停止路径仍读取但不使用");break;
        }
        case 0x56A8: {
            const std::string ev="IDA 0x9279E60 → 0x8CA7040；成功时保存 +0x50/+0x54，触发 RedDot_Customizing（0x232）";
            m.name+=" 外观自定义相关响应";r.u("结果码（0=成功；其他枚举待确认）",2,ev,false);
            r.raw("自定义状态",0x50,4,ev);r.raw("自定义状态",0x54,4,ev);break;
        }
        case 0xE274: {
            const std::string ev="IDA 0x931B820 / 0x90A4880 → 0x8C7F4E0；Community_Update（0x3F8）";
            m.name+=" 社区双列表同步";auto n=r.count("社区列表 A 数量",ev,31);
            for(uint32_t i=0;i<n;++i){auto p="社区 A "+std::to_string(i);auto flags=r.u(p+" 可选字段位图",1,ev);r.flagsKnown(flags,1);
                r.u(p+" 服务器编号",2,ev+"；0x8C7E6C0 按 u16 查询世界服务器列表 +0x60 并取得名称");
                r.u(p+" 记录键（身份含义待确认）",8,ev+"；消费者以此值作为社区表键",false);
                r.stringBytes(p+" 字符串 +0x18",ev);
                r.raw(p,0x28,1,ev);r.raw(p,0x29,1,ev);r.raw(p,0x2c,4,ev);r.raw(p,0x30,4,ev);r.raw(p,0x34,1,ev);
                r.boolean(p+" 清空时间标志（业务含义待确认）",ev+"；为 true 时消费者将记录时间置零，位游标跨条目共享");
                r.u(p+" 时间（毫秒；用途待确认；0 为空值）",8,ev+"；标志为 false 时非零值乘 10000 加日历纪元常量",false);
                if(flags&1)r.stringBytes(p+" 可选字符串 +0x40",ev);
            }
            n=r.count("社区列表 B 数量",ev,11);
            for(uint32_t i=0;i<n;++i){auto p="社区 B "+std::to_string(i);r.raw(p,8,2,ev);r.raw(p,0x10,8,ev);r.stringBytes(p+" 字符串 +0x18",ev);}
            break;
        }
        case 0x8D4D: {
            const std::string ev="IDA 0x928BBE0 / 0x8F94BA0";
            m.name+=" 物品操作响应（具体操作待确认）";
            r.u("物品操作结果码（0=成功；其他枚举待确认）",2,ev,false);
            r.u("物品实例编号",8,ev+"；查玩家 +0x770 表，实例 +8 为 Item 配置键；0x928BCDA → 0x8DC97F0 / 0x88F6C50");break;
        }
        case 0x5648: {
            const std::string ev="IDA 0x92DBB40 / 0x9275FD0；与 0x9276120 共用药水设置表，事件 AutoUse_PotionDataChanged";
            m.name+=" 自动使用药水设置";
            auto n=r.count("药水设置条目数量",ev,9);
            for(uint32_t i=0;i<n;++i){auto p="药水设置 "+std::to_string(i);
                auto kind=r.u(p+" 类别（枚举待确认）",1,ev,false);
                const std::string use=ev+"；0x8DB47A0 查询类别 1；0x8DB4890 将 +4/+8 作为两个药水候选，库存及条件不足时允许选择备用";
                r.u(p+(kind==1?" 首选药水编号":" 编号 A（语义待确认）"),4,use,kind==1);
                r.u(p+(kind==1?" 备用药水编号":" 编号 B（语义待确认）"),4,use,kind==1);}
            break;
        }
        case 0x564B: {
            const std::string ev="IDA 0x92DBC50 / 0x9276270 → 0x8DB6DE0(enable=1)；事件 AutoUse_ItemDataChanged；0x88F6C50 的 Item 表，虚表 0xDE7F7B8 / 名称 0x88CCBA0";
            m.name+=" 已启用自动使用的道具列表";
            auto n=r.count("自动使用道具数量",ev,4);
            for(uint32_t i=0;i<n;++i)r.u("自动使用道具 "+std::to_string(i)+" Item 配置编号",4,ev);
            break;
        }
        case 0x8D44: {
            const std::string ev="IDA 0x928B8D0 / 0x8570EC0 / 0x90827B0",p="状态设置对象";
            m.name+=" 状态设置（字段含义待确认）";
            r.rawBool(p,8,ev);
            r.boolean("启用备用药水选择",ev+"；内嵌对象 +9 → 玩家 +0x1FE0；0x8DB4890 为 false 时保留首选，为 true 时允许检查备用候选",true);
            r.i32(p+" +0xC（语义待确认；负值不更新）",ev+"；0x9082855 按有符号值判断",false);
            r.rawBool(p,0x10,ev);r.raw(p,0x11,1,ev);r.rawBool(p,0x12,ev);r.rawBool(p,0x13,ev);r.raw(p,0x14,1,ev);
            for(auto o:{0x15,0x16,0x17,0x18,0x19,0x1a,0x1b})r.rawBool(p,o,ev);
            r.raw(p,0x1c,1,ev);break;
        }
        case 0x8D53: {
            const std::string ev="IDA 0x92FF7E0 / 0x909FC20 → 0x8ED1E10；0x893B540 虚表 0xDE81BF8，0x890C0E0 返回 SeasonSchedule；0x98A7CA0 以转换后的字符串标识和整数为复合键查表";
            m.name+=" SeasonSchedule 分类索引";
            auto n=r.count("分类条目数量",ev,16);
            for(uint32_t i=0;i<n;++i){auto p="分类条目 "+std::to_string(i);r.u(p+" 分类键（枚举待确认）",1,ev,false);
                for(auto suffix:{"A","B","C"}){r.stringBytes(p+" SeasonSchedule 索引 "+suffix+" 字符串",ev,true);r.u(p+" SeasonSchedule 索引 "+suffix+" 整数",4,ev);}}
            break;
        }
        case 0x8D58: {
            const std::string ev="IDA 0x9300140 / 0x90A0E20";
            m.name+=" 分类进度条目（用途待确认）";
            auto n=r.count("进度条目数量",ev,27);
            for(uint32_t i=0;i<n;++i){auto p="进度条目 "+std::to_string(i);r.raw(p,8,1,ev);r.raw(p,9,1,ev);
                r.i32(p+" 比率分子（业务含义待确认）",ev+"；0x90A0EC5/0x90A0EE2，与 +0x10 相除再乘 100",false);
                r.i32(p+" 比率分母（业务含义待确认）",ev+"；0x90A0ECD，小于等于 0 时比率为 0",false);
                r.raw(p,0x18,8,ev);r.stringBytes(p+" 字符串",ev);r.rawBool(p,0x30,ev);r.raw(p,0x34,4,ev);r.raw(p,0x38,4,ev);}
            break;
        }
        case 0xE25A: {
            const std::string ev="IDA 0x92E2ED0 / 0x92A18E0；0xAA27120 建立 SDK 对象，虚表 0xE0F7300 +0x28 → 0xAA27060；0xAA26810 加载 NCGuardSDK 的 bb64.dll";
            m.name+=" NCGuard SDK 数据响应";
            r.u("SDK 响应结果码（0=成功；其他枚举待确认）",2,ev,false);
            r.blob("NCGuard SDK 数据（内部字段待确认）",ev);break;
        }
        case 0xE215: {
            const std::string ev="IDA 0x9317430 / 0x909A1B0 → 0x8E83C60；Post_PostListUpdate / Post_PostCount";
            m.name+=" 邮件列表响应";r.u("邮件结果码（0=成功；其他枚举待确认）",2,ev,false);
            r.u("邮件列表类型（枚举映射待确认）",1,ev,false);r.raw("邮件响应",8,4,ev);r.raw("邮件响应",0xc,4,ev);
            auto n=r.count("邮件条目数量",ev,34);
            for(uint32_t i=0;i<n;++i){auto p="邮件条目 "+std::to_string(i);
                r.raw(p,8,1,ev);r.raw(p,9,1,ev);
                r.u(p+" Post 配置编号",4,ev+"；0x9098BE6 查找 0x90AEBE0 的 Post 表（虚表 0xDEE8928，名称函数 0x9030980）");
                r.raw(p,0x10,8,ev);r.stringBytes(p+" 模板参数字符串",ev+" → 0x90982E0 格式化输入");
                r.rawBool(p,0x28,ev);r.rawBool(p,0x29,ev);
                for(auto timeLabel:{" 时间 A"," 时间 B"})r.u(p+timeLabel+"（毫秒；用途待确认；0 为空值）",8,ev+"；0x9099106 / 0x9099123 转日历 tick",false);
                auto count=r.count(p+" 附加条目数量",ev,20);
                for(uint32_t j=0;j<count;++j){auto q=p+" 附加条目 "+std::to_string(j);r.raw(q,8,4,ev);r.raw(q,0x10,8,ev);
                    r.u(q+" 时间（毫秒；用途待确认；0 为空值）",8,ev+"；0x9099413 转日历 tick",false);}
                r.raw(p,0x50,2,ev);
            }break;
        }
        case 0xE263: {
            const std::string ev="IDA 0x92A1EF0；Trade_UpdateServerState（事件 0x3E4）";
            m.name+=" 交易服务器状态";
            r.boolean("交易组 0/1 状态（具体业务分组待确认）",ev+"；bit 0 → 对象 +0x29，控制交易策略及收益请求");
            r.boolean("交易组 2/3 状态（具体业务分组待确认）",ev+"；bit 1 → 对象 +0x28");
            r.u("交易组 0 服务器编号",2,ev+"；+0x2A → 0x8F56560 → /trade/v1.0 的 game_server_id");
            r.u("交易组 1 服务器编号",2,ev+"；+0x2C → 0x8F56560 → /trade/v1.0 的 game_server_id");break;
        }
        case 0x561D: {
            const std::string ev="IDA 0x9274080；MyPlayer_UpdateEquipItemLevel（0x6C）";
            m.name+=" 装备等级更新";
            r.u("装备等级原始值（缩放待确认）",4,ev+" → 玩家 +0x2E8；0x8D5C830 比较更新前后阈值",false);
            r.u("装备等级相关值 B（用途待确认）",4,ev+" → 玩家 +0x2EC",false);break;
        }
        case 0x5633: {
            const std::string ev="IDA 0x92C5770 / 0x905C650；GodStone_Loaded（0xD4）";
            m.name+=" 神石列表";auto n=r.count("神石条目数量",ev,8);
            for(uint32_t i=0;i<n;++i){auto p="神石条目 "+std::to_string(i);
                r.u(p+" GodstoneData 配置编号",4,ev+"；同表更新 0x8DCF050 → 0x88F5150 / 虚表 0xDE7F538，名称 0x88CD480");
                r.i32(p+" 持有数值（具体含义待确认）",ev+"；0x905C6F0 movsxd，符号扩展存入 +0x3B8 表",false);}
            break;
        }
        case 0x56AC: {
            const std::string ev="IDA 0x927A1B0；同表增量 0x927A370 → 0x8D71600，WorldMap_OpenFogGroup_Changed（0x29E）";
            m.name+=" 已开启地图迷雾组列表";auto n=r.count("已开启迷雾组数量",ev,1);
            for(uint32_t i=0;i<n;++i)r.u("已开启迷雾组 "+std::to_string(i)+" 配置编号",1,ev+"；玩家 +0x28 对象的 +0x130；查表 0x8D90160 / 名称 0x8D6D6F0");
            break;
        }
        case 0x56B6: {
            const std::string ev="IDA 0x92E3AC0 / 0x90AB060；AccountContentsInfo_Changed（0x456）";
            m.name+=" 账号内容赛季记录";auto n=r.count("账号内容条目数量",ev,27);
            for(uint32_t i=0;i<n;++i){auto p="账号内容 "+std::to_string(i);
                r.u(p+" 类别（枚举待确认）",1,ev+" → 0x8F98A70 复合键第一项",false);
                r.u(p+" SeasonScheduleGroup 配置编号",4,ev+" → 0x90AD140；虚表 0xDEE87E8 / 名称 0x90309A0");
                r.u(p+" SeasonSchedule 整数查找键",4,ev+"；组表字符串及此值交给 0x98A7CA0");
                r.raw(p,0x20,2,ev);r.raw(p,0x28,8,ev);r.raw(p,0x30,4,ev);r.raw(p,0x34,4,ev);}
            break;
        }
        case 0x5672: {
            const std::string ev="IDA 0x92DF2C0 / 0x9277E20 → Wing_Clear / Wing_Add";
            m.name+=" 翅膀列表";auto n=r.count("翅膀条目数量",ev,5);
            for(uint32_t i=0;i<n;++i){auto p="翅膀条目 "+std::to_string(i);
                r.u(p+" 翅膀编号",4,ev);r.raw(p,0xc,1,ev);}
            break;
        }
        case 0x5679: {
            const std::string ev="IDA 0x92DFEF0 / 0x9278310 → Title_Acquired";
            m.name+=" 称号列表";auto n=r.count("已获得称号数量",ev,4);
            for(uint32_t i=0;i<n;++i)r.u("已获得称号 "+std::to_string(i)+" 编号",4,ev);
            n=r.count("称号关联表数量",ev,5);
            for(uint32_t i=0;i<n;++i){auto p="称号关联 "+std::to_string(i);
                r.u(p+" 分类键（枚举待确认）",1,ev+" → 0x8FA40B0 / 玩家 +0x93B0",false);
                r.u(p+" 关联值（用途待确认）",4,ev,false);}
            break;
        }
        case 0x5682: {
            const std::string ev="IDA 0x9278B40 → 玩家 +0x90 成就对象";
            m.name+=" 成就相关数值";
            r.u("成就数值 A（用途待确认）",2,ev+" +0xD8",false);
            r.u("成就数值 B（用途待确认）",2,ev+" +0xDC",false);break;
        }
        case 0x5683: {
            const std::string ev="IDA 0x92E0860 / 0x908B9C0 → 0x8BDEFF0 / 0x8BDEDD0";
            m.name+=" 成就初始列表";auto n=r.count("成就条目数量",ev,17);
            for(uint32_t i=0;i<n;++i){auto p="成就条目 "+std::to_string(i);
                r.u(p+" 成就配置编号",4,ev+" → 0x8FF3EC0 / 0x8B42260");r.raw(p,8,4,ev);
                r.u(p+" 时间（毫秒；用途待确认；0 为空值）",8,ev+"；0x908BDD7 乘 10000 并加日历纪元常量",false);
                r.raw(p,0x18,1,ev);}
            n=r.count("成就组条目数量",ev,5);
            for(uint32_t i=0;i<n;++i){auto p="成就组 "+std::to_string(i);
                r.u(p+" 配置编号",4,ev+" → 0x8FF3900");auto bits=r.var(p+" 布尔项数量",ev);
                const auto available=8-r.boolBit;
                if(bits>4096 || (bits>available && (bits-available+7)/8>r.b.size()-r.pos))
                    throw std::runtime_error("布尔数组数量超过剩余字节或分析限制");
                for(uint32_t j=0;j<bits;++j)r.boolean(p+" 标志 "+std::to_string(j)+"（用途待确认）",ev+" / 0x8557F00；共享位游标跨条目延续");}
            break;
        }
        case 0x5684: {
            const std::string ev="IDA 0x92E0FB0 / 0x908BE60 → 0x8BDF670";
            m.name+=" 成就相关状态列表";auto n=r.count("状态条目数量",ev,17);
            for(uint32_t i=0;i<n;++i){auto p="状态条目 "+std::to_string(i);
                r.u(p+" 外层键（用途待确认）",4,ev+"；部分类型触发 Achievement_ChangedState / WithGroupId",false);
                r.raw(p,8,4,ev);
                r.u(p+" 时间（毫秒；用途待确认；0 保留为空值）",8,ev+"；0x908C070 乘 10000，再加日历纪元常量；零不转换",false);
                r.raw(p,0x18,1,ev);
            }break;
        }
        case 0xFFA9:
            m.name+=" 深渊限制更新";
            r.u("深渊限制相关值 A（具体角色待确认）",8,"IDA 0x92A958E → 对象 +0x9618；事件 0x454=AbyssLimit_Changed",false);
            r.u("深渊限制相关值 B（具体角色待确认）",8,"IDA 0x92A95A1 → 对象 +0x9620；事件枚举 RVA 0xD789000",false);break;
        case 0x3656:
            m.name+=" 战斗力更新";
            r.u("战斗力相关值 A（具体角色待确认）",8,"IDA 0x92635A1 → 对象 +0x658；事件 0x46B=MyPlayer_UpdateCombatPower",false);
            r.u("战斗力相关值 B（具体角色待确认）",8,"IDA 0x92635B4 → 对象 +0x660；事件枚举 RVA 0xD789170",false);break;
        case 0x364A: {
            const std::string ev="IDA 0x92C2A60 / 0x903CE00";m.name+=" 实体属性列表";
            r.var("实体编号",ev+" → 0x94B0BD0 实体查找");auto n=r.count("属性数量",ev,6);
            for(uint32_t i=0;i<n;++i){auto p="属性 "+std::to_string(i);auto key=r.u(p+" 类型",2,ev+"；EStat 枚举表 RVA 0xDD58940",false);
                if(key<statNames.size()){r.out.fields.back().value+=" / "+std::string(statNames[key]);r.out.fields.back().meaningKnown=true;p+=" "+std::string(statNames[key]);}
                r.i32(p+(key==0xC1?" 32 位占位值（客户端使用末尾 64 位值）":" 原始值（单位/缩放待确认）"),ev,false);}
            r.u("HPMax 64 位原始值",8,ev+"；类型 193=HPMax，非零时提交此值，零时不更新该属性");break;
        }
        case 0x8D00: {
            const std::string ev="IDA 0x92FBB10 / 0x903D400";m.name+=" 实体资源更新";
            r.var("实体编号",ev+" → 0x94B0BD0 实体查找");auto flags=r.u("属性表位图",1,ev);r.flagsKnown(flags,3);
            static constexpr const char* names[]={"HP","MP","AP","SP","DP","OP","FP","MaxHP","MaxMP","MaxAP","MaxSP","MaxDP","MaxOP","MaxFP"};
            const std::string enumEv="枚举表 0xDD54E60 ECharPointType；0x903D64C/0x903D93E 转换与 0xDCE2670 EResource 对应";
            for(unsigned group=0;group<2;++group)if(flags&(uint64_t{1}<<group)){
                auto p=group?"64 位资源":"32 位资源";auto n=r.count(std::string(p)+"数量",ev,group?9:5);
                for(uint32_t i=0;i<n;++i){auto prefix=std::string(p)+" "+std::to_string(i);auto key=r.u(prefix+" 类型",1,enumEv,false);
                    const bool known=key<std::size(names);if(known){r.out.fields.back().value+=" / "+std::string(names[key]);r.out.fields.back().meaningKnown=true;}
                    auto name=prefix+" "+(known?names[key]:"未知资源")+" 数值";
                    if(group)r.u(name,8,ev+" / 0x8B45DD0 保存值",known);else r.i32(name,ev+"；处理器符号扩展至 64 位",known);
                }
            }break;
        }
        case 0x371A:case 0x371B:case 0x371C:case 0x371D: {
            const bool delta=opcode==0x371D, initial=opcode==0x371A;
            const std::string ev=delta?"IDA 0x92C8270":initial?"IDA 0x92C7C20":"IDA 0x92C7F10";
            m.name+=delta?" 普通移动位置增量":initial?" 普通移动停止":opcode==0x371B?" 普通移动开始":" 普通移动位置快照";
            r.var("实体编号",ev+" -> 实体查找");auto flags=r.u("可选字段位图",1,ev);
            if(initial) {moveStateField(r,ev);if(flags&1)r.u("移动子状态（枚举待确认）",1,ev+"；_move_substate",false);}
            else if(flags&1)r.boolean("跑步状态",ev+"；_is_running / 0xEDF9BC8..0xEDF9C38",true);
            if(delta) {
                if(flags&2)r.i8("delta X","IDA 0x90454FD / 0x9045750");
                if(flags&4)r.i8("delta Y","IDA 0x90454E7 / 0x9045737");
                if(flags&8)r.i8("delta Z","IDA 0x90454E2 / 0x9045730");
            }else {r.f32("X",ev+" / 位置处理器");r.f32("Y",ev+" / 位置处理器");r.f32("Z",ev+" / 位置处理器");}
            r.angle(initial?"朝向":"移动方向",ev+"；_rotate_dir / _move_dir",true);
            if(!initial)r.angle("朝向 yaw","IDA 0x9044C9D / 0x904554C",true);
            const auto nested=delta?0x10:2;
            if(flags&nested)localMovementFields(r,uint16_t(opcode));
            if(flags&(delta?0x20:4))r.boolean(initial?"移动惩罚启用":"航位推算启用",ev+"；_penalty_on / _dead_reckoning_on，消息级共享 bool",true);
            if(initial && (flags&8))r.u("移动惩罚时长（毫秒）",8,ev+"；_penalty_ms / 0xEDF9B90 / 镜像桥 0x85B70A0");
            if(flags & ~(delta?0x3f:initial?0x0f:7)){stop=true;m.status="出现未定义的可选字段位";}
            break;
        }
        case 0x371E:case 0x371F:case 0x3720: {
            const bool start=opcode==0x371E, delta=opcode==0x3720;
            const std::string ev=start?"IDA 0x92C8590 / 0x85B7D10":delta?"IDA 0x92C8B30 / 0x85B84C0":"IDA 0x92C8830 / 0x85B8180";
            m.name+=start?" 重力移动开始":delta?" 重力移动增量":" 重力移动位置快照";
            r.var("实体编号",ev+" → 0x94B0BD0");auto flags=r.u("可选字段位图",1,ev);r.flagsKnown(flags,start?31:delta?15:3);
            if(start && (flags&1))r.u("重力模式（枚举待确认）",1,ev+"；_gravity_mode",false);
            if(delta){
                if(flags&1)r.i8("delta X","IDA 0x9046DC0 / 0x9047038..0x9047060");
                if(flags&2)r.i8("delta Y","IDA 0x9046DC0 / 有符号字节加到网络位置基准");
                r.i8("delta Z","IDA 0x92C8B30 / 始终存在；0x9046DC0 符号扩展");
            }else for(auto axis:{"X","Y","Z"})r.f32(axis,ev+"；_pos → 0x9045DD0 / 0x9046630，单位待确认");
            r.angle("朝向 yaw","IDA 0x9045DD0 / 0x9046630 / 0x9046EF6；u16 × 360 / 65536",true);
            for(auto axis:{"X","Y","Z"})r.f32(std::string("移动速度 ")+axis,ev+"；_move_velocity_3d，单位待确认");
            if(flags&(start?2:delta?4:1))localMovementFields(r,uint16_t(opcode));
            if(start){
                if(flags&4)r.boolean("向上跳跃",ev+"；_is_jump_up",true);
                if(flags&8)r.u("跳板 ID",8,ev+"；_springboard_id");
            }
            if(flags&(start?16:delta?8:2))r.boolean("航位推算启用",ev+"；_dead_reckoning_on，共享布尔位流",true);
            break;
        }
        case 0x372E:case 0x372F: {
            const bool start=opcode==0x372E;
            const std::string ev=start?"IDA 0x92CA0A0 / 运行时镜像桥 0x85BADD0 / 0xEDF9FF0":"IDA 0x92CA3E0 / 运行时镜像桥 0x85BB320 / 0xEDFA028";
            m.name+=start?" 前向攻击位移开始":" 前向攻击位移位置快照";
            r.var("实体编号",ev);auto flags=r.u("可选字段位图",1,ev);r.flagsKnown(flags,start?127:7);
            if(start){
                if(flags&1)r.u("技能 ID",4,ev+"；_skill_id");
                if(flags&2)moveStateField(r,ev);
                if(flags&4)r.u("移动子状态（枚举待确认）",1,ev+"；_move_substate",false);
            }
            for(auto axis:{"X","Y","Z"})r.f32(axis,ev+"；_pos，区别于 _target_pos，单位待确认");
            for(auto axis:{"X","Y","Z"})r.f32(std::string("移动速度 ")+axis,ev+"；_move_velocity_3d，单位待确认");
            r.floatEncodedAngle("朝向 yaw",ev+"；_rotate_dir，线上 float32；消费者 0x904C5B3..0x904C5D9 / 0x904CF0A..0x904CF30：截断至 i32，取低 16 位 × 360 / 65536 度");
            if(flags&(start?8:1))localMovementFields(r,uint16_t(opcode));
            if(start){
                if(flags&16)r.u("目标实体编号",4,ev+"；_target_key，线上固定 u32");
                if(flags&32)for(auto axis:{"X","Y","Z"})r.f32(std::string("目标位置 ")+axis,ev+"；_target_pos，单位待确认");
            }
            if(flags&(start?64:2))r.boolean("航位推算启用",ev+"；_dead_reckoning_on",true);
            if(!start && (flags&4))r.boolean("位移结束",ev+"；_move_end，共享 bool 位流",true);
            break;
        }
        case 0x373E:case 0x373F:case 0x3740: {
            const std::string ev=opcode==0x373E?"运行时代码 0x926CD50 / CharMoveStartReject_RS":opcode==0x373F?"运行时代码 0x926CFB0 / CharMoveUpdateReject_RS":"运行时代码 0x926D210 / CharMoveStopReject_RS";
            m.name+=opcode==0x373E?" 移动开始被拒绝 / 服务器位置校正":opcode==0x373F?" 移动更新被拒绝 / 服务器位置校正":" 移动停止被拒绝 / 服务器位置校正";
            r.var("实体编号",ev+" → 0x94B0BD0 按键查找");
            moveStateField(r,ev);
            r.u("移动子状态（枚举待确认）",1,ev+"；_move_substate",false);
            for(auto axis:{"X","Y","Z"})r.f32(axis,ev+"；_position → 0x93C5C00 → 0x93B71F0 → 0x93B50F0，单位待确认");
            r.u("拒绝原因（枚举待确认）",2,ev+"；_reason：线上 u16，反射镜像为 u32",false);break;
        }
        case 0x3741:case 0x3742: {
            m.name+=opcode==0x3741?" 疾跑开启响应":" 疾跑关闭响应";
            const std::string ev="运行时代码 0x926D470 / 分发 0x9261B20；CharMoveSprintOn_RS / CharMoveSprintOff_RS";
            r.var("实体编号",ev+"；继承实体头；消费者使用当前自身角色，未按此键查找");
            if(opcode==0x3741)r.u("结果码（0=成功；其他值待确认）",2,ev+"；_result 线上 u16",false);
            break;
        }
        case 0x3633:m.name+=" 自身角色出现";selfAppearFields(r);break;
        case 0x3634: {
            const std::string ev="IDA 0x92BE790 / EnvObjSpawn_NT 0xEDF8CE8",p="对象";m.name+=" 环境对象出现";r.var("实体编号",ev);auto f=r.u("可选字段位图",1,ev);r.flagsKnown(f,15);identityFields(r,"基础结构");
            for(auto axis:{"X","Y","Z"})r.f32(std::string("位置 ")+axis,ev+"；_pos / Vec3，单位待确认",false);
            r.u("来源生成键",4,ev+"；_origin_spawn_key");r.f32("对象朝向（单位待确认）",ev+"；_dir",false);r.raw(p,0x70,1,ev);r.rawBool(p,0x71,ev);r.raw(p,0x72,1,ev);r.raw(p,0x78,8,ev);r.raw(p,0x80,4,ev);
            if(f&1)r.raw(p,0x88,8,ev);r.raw(p,0x90,4,ev);if(f&2)r.raw(p,0x98,8,ev);if(f&4)r.raw(p,0xa0,4,ev);r.raw(p,0xa4,1,ev);r.raw(p,0xa5,1,ev);if(f&8)r.raw(p,0xa8,4,ev);r.rawVar(p,0xac,ev);break;
        }
        case 0x3641: {
            m.name+=" NPC/怪物出现";
            // Two verified wire layouts. Only publish a uniquely complete parse;
            // never let a failed candidate contaminate the nearby object model.
            GameMessage selected;size_t matches=0;
            for(bool current:{true,false}){
                GameMessage candidate=m;Reader probe{bytes,candidate,r.pos,r.boolPos,r.boolBit};
                try{npcAppearFields(probe,current);if(probe.pos==bytes.size()){selected=std::move(candidate);++matches;}}
                catch(const std::runtime_error&){}
            }
            if(matches!=1)throw std::runtime_error(matches?"NPC 包版本存在歧义":"NPC 包不符合已验证的新版或旧版格式");
            m=std::move(selected);r.pos=bytes.size();break;
        }
        case 0x3645: {
            const std::string ev="IDA 0x92C10B0 / CharAppear_NT 0xEDF90A0",p="对象";m.name+=" 玩家角色出现";r.var("实体编号",ev);auto f=r.u("可选字段位图",4,ev);r.flagsKnown(f,0x7ffffff);
            identityFields(r,"基础结构");stateFields(r,"状态结构");if(f&1)r.u("服务器 ID",2,ev+"；_server_id，线上 u16");if(f&2)affiliationFields(r,"关联结构");
            auto n=r.count("附加状态数量",ev,23);for(uint32_t i=0;i<n;++i)effectFields(r,"附加状态 "+std::to_string(i));
            n=r.count("复合条目数量",ev,21);for(uint32_t i=0;i<n;++i)equipmentFields(r,"复合条目 "+std::to_string(i));
            if(f&4)r.raw(p,0x1a8,1,ev);if(f&8)r.raw(p,0x1a9,1,ev);if(f&16)r.rawBool(p,0x1aa,ev);if(f&32)r.rawBool(p,0x1ab,ev);if(f&64)r.raw(p,0x1ac,1,ev);
            if(f&128)for(auto o:{0x1b8,0x1bc,0x1c0})r.raw(p,o,4,ev);if(f&256)r.raw(p,0x1c8,8,ev);if(f&512)r.raw(p,0x1d0,2,ev);if(f&1024)r.raw(p,0x1d2,2,ev);if(f&2048)r.raw(p,0x1d8,8,ev);if(f&4096)r.rawBool(p,0x1e0,ev);if(f&8192)r.raw(p,0x1e1,1,ev);if(f&16384)r.rawBool(p,0x1e2,ev);
            r.u("角色数据库 ID",8,ev+"；_dbid +0x1E8");initialStatFields(r,ev+" / 0x903671C / 0x903729B");
            r.u("等级",4,ev+"；_level +0x200");r.u("征服者等级",4,ev+"；_conqueror_level +0x204");r.u("装备等级",4,ev+"；_equip_item_level +0x208");if(f&32768)r.raw(p,0x20c,4,ev);if(f&65536)r.raw(p,0x210,4,ev);if(f&131072)r.raw(p,0x214,4,ev);
            n=r.count("字节数组数量",ev,1);for(uint32_t i=0;i<n;++i)r.u("字节数组["+std::to_string(i)+"]",1,ev,false);
            if(f&0x40000)r.raw(p,0x228,1,ev);if(f&0x80000)r.raw(p,0x22c,4,ev);if(f&0x100000)optionalFieldsB(r,"可选结构");if(f&0x200000)r.raw(p,0x268,4,ev);if(f&0x400000)r.u("拟态 NPC 模板 ID",4,ev+"；_mimic_npc_data_id +0x26C；0x9036410 非零时创建拟态对象");if(f&0x800000)r.raw(p,0x270,4,ev);if(f&0x1000000)r.raw(p,0x274,4,ev);if(f&0x2000000)r.rawBool(p,0x278,ev);if(f&0x4000000)r.raw(p,0x27c,4,ev);
            r.u("战斗力",8,ev+"；_combat_power +0x280");r.raw(p,0x288,1,ev);r.rawBool(p,0x289,ev);break;
        }
        case 0x3650: {
            const std::string ev="ViewChar_RS；分发表 0x926321C → 0x92C43D0；成功消费者 0x9090AF0；反射 0xEDF9308";
            m.name+=" 查看角色资料响应";
            m.queryResult=uint16_t(r.u("资料查询结果（0=成功；其他值待确认）",2,ev+"；0x9263238..0x926324B",false));
            identityFields(r,"资料角色");
            r.u("资料角色等级",4,ev+"；原生 +0x40 / _level");
            r.u("资料角色征服者等级",4,ev+"；原生 +0x44 / _conqueror_level");
            r.u("资料角色装备等级",4,ev+"；原生 +0x48 / _equip_item_level");
            r.u("资料角色出生服务器 ID",2,ev+"；原生 +0x4C / _born_server_id，线上 u16");
            r.u("资料角色当前服务器 ID",2,ev+"；原生 +0x4E / _current_server_id，线上 u16");
            r.boolean("资料角色在线",ev+"；原生 +0x50 / _online",true);
            r.u("资料角色称号 ID",4,ev+"；原生 +0x54 / _title_id");
            affiliationFields(r,"资料角色");
            m.viewCharPrefixComplete=true;
            viewCharEquipmentFields(r);
            stop=true;m.status="资料和时装染色已解析；外观定制及养成尾部未展开";
            break;
        }
        case 0x3635:
            r.var("实体编号","IDA 0x92624F5");r.u("状态 A（语义待确认）",1,"IDA 0x9262508",false);
            r.u("状态 B（语义待确认）",1,"IDA 0x926251B",false);r.var("附加值（语义待确认）","IDA 0x9262528",false);break;
        case 0x3638:
            r.var("实体编号","IDA 0x926266B");r.var("附加编号（语义待确认）","IDA 0x9262677",false);r.u("状态（语义待确认）",1,"IDA 0x9262689",false);break;
        case 0x363B:r.var("实体编号","IDA 0x92627D2 / 0x9262810");break;
        case 0x3646: {
            const std::string ev="0x9262DBD → 0x92C24F0 → 0x90E2B30；CharBarrierInfo_NT 0xEDF90D8 / BarrierInfo 0xEE1E020";
            m.name+=" 角色屏障信息";r.var("实体编号",ev);auto n=r.count("屏障条目数量",ev,2);
            for(uint32_t i=0;i<n;++i){auto p="屏障条目 "+std::to_string(i);r.var(p+" 状态实例 UID",ev+"；_uid，与实体键共同查找 +0x28B8 状态集合");r.var64(p+" 剩余屏障值",ev+"；_remain_barrier → 0x8B3BE70 更新资源",true);}break;
        }
        case 0x3649: {
            const std::string ev="0x9262EA4 → 0x92C27C0 → 0x903CBF0；CharAllStats_RS 0xEDF9180";
            m.name+=" 自身全部属性响应";r.u("结果码（0=成功；其他值待确认）",2,ev,false);initialStatFields(r,ev);
            r.u("HPMax 64 位覆盖值（0 不应用）",8,ev+"；_stat_hpmax；0x903CBF0 仅当属性 193 / HPMax 存在且此值非零时替代其 i32 值");break;
        }
        case 0x3647:
            m.name+=" 玩家角色离开";r.var("实体编号","IDA 0x9262E42 / 0x903C8B0 → 按键移除对象");r.u("离开原因（枚举待确认）",1,"IDA 0x9262E55 / CharDisappear_NT 0xEDF9110",false);r.var("传送效果 ID","IDA 0x9262E62 / _teleport_effect_id");break;
        case 0x3721:r.var("实体编号","IDA 0x9263C07 / 0x9263C4A");break;
        case 0x3746:
            m.name+=" 最近地面高度通知";
            r.var("实体编号","IDA 0x92650AA → 0x9330E30；继承实体头；本分支消费者未按此键查找对象");
            r.f32("最近地面高度 Z","IDA 0x92650BD / 0x92650E4；写自身移动组件 +0xF4/+0xF8，0x8E38D70 将其用于飞行上限；与 LastOnGroundPosZ_NT._pos_z 反射一致，单位待确认");break;
        case 0x3722:r.var("编号（语义待确认）","运行时代码 / IDA 0x926CC0F；反编译栈有警告，未据此命名业务含义",false);break;
        case 0x3729:case 0x372A: {
            bool delta=opcode==0x372A;std::string ev=delta?"IDA 0x92C9440":"IDA 0x92C9350";
            m.name+=delta?" 位置增量 / 方向向量":" 位置 / 方向向量";
            r.var("实体编号",ev);auto flags=r.u("可选字段位图",1,ev);
            if(flags&1)r.boolean("移动状态标志（语义待确认）",ev);
            if(delta){if(flags&2)r.i8("delta X","IDA 0x904A449");if(flags&4)r.i8("delta Y","IDA 0x904A43F");if(flags&8)r.i8("delta Z","IDA 0x904A44E");}
            else {r.f32("X",ev);r.f32("Y",ev);r.f32("Z",ev);}
            r.f32("方向向量 X（用途推断）",ev,false);r.f32("方向向量 Y（用途推断）",ev,false);r.f32("方向向量 Z（用途推断）",ev,false);
            r.angle("朝向 yaw","IDA 0x9049E6B / 0x904A322",true);
            if(flags&(delta?0x10:2))r.boolean("插值状态标志（语义待确认）",ev);
            if(flags&~(delta?0x1f:3)){stop=true;m.status="出现未定义的可选字段位";}break;
        }
        case 0x8A33:
            m.name+=" 实体公会更新";r.var("实体编号","IDA 0x9283EBE / 0x90D4ED6 → 0x94B0BD0");affiliationFields(r,"公会结构");break;
        case 0x382C: {
            const std::string ev="IDA 0x92CFC50 / 0x9055CA0 → 0x93F53A0；事件 Object_AbnormalRemoved";
            m.name+=" 异常状态移除";r.var("实体编号",ev);auto n=r.count("移除条目数量",ev,3);
            for(uint32_t i=0;i<n;++i){auto p="移除条目 "+std::to_string(i);auto flags=r.u(p+" 位图",1,ev);r.flagsKnown(flags,7);
                r.var(p+" 状态实例编号",ev+"；与实体组合为状态查找键");
                static constexpr const char* names[]={"None","Timeout","Die","Cancellation","Replace","Passive","Condition","Etc","AI_Reset","EventFunction","ClassChange","Dispel","SelfRemove","Duel","DungeonPcCount","Cutscene","AbnormalClear","AbyssArtifact","DiePvP","DiePvE","Max"};
                auto type=r.u(p+" 移除原因",1,ev+"；AbnormalRemoveInfo 0xEE1DFE8；_expire_reason +0xC → EAbnormalExpireReason 0xEE2DA70",false);
                if(type<std::size(names)){r.out.fields.back().value+=" / "+std::string(names[type]);r.out.fields.back().meaningKnown=true;}
                if(flags&1)r.var(p+" 驱散施放者实体键",ev+"；_dispel_caster_key +0x10");
                if(flags&2)r.u(p+" 驱散技能 ID",4,ev+"；_dispel_skill_id +0x14");
                if(flags&4)r.u(p+" 驱散技能效果 ID",4,ev+"；_dispel_skill_effect_id +0x18");}
            break;
        }
        case 0x382A: {
            const std::string ev="IDA 0x92CF810";m.name+=" 实体附加状态列表";r.var("实体编号",ev);auto n=r.count("状态条目数量",ev,23);
            for(uint32_t i=0;i<n;++i)effectFields(r,"状态条目 "+std::to_string(i));break;
        }
        case 0x380F:case 0x3813: {
            const bool list=opcode==0x3813;const std::string ev=list?"IDA 0x92CEB90":"IDA 0x926E520";
            m.name+=list?" 技能获取状态列表":" 技能获取状态";
            const std::string lookupEv="IDA 0x90577CC / 0x9058783 → 0x98AAFC0；表虚函数 0x8AB9A40 返回 SkillAcquireData";
            const auto n=list?r.count("条目数量",ev,11):1;
            for(uint32_t i=0;i<n;++i){auto p="条目 "+std::to_string(i);if(list)r.u(p+" 技能获取配置键",4,lookupEv);
                auto entryFlags=r.u(p+" 位图",1,ev);r.flagsKnown(entryFlags,1);
                r.u(p+(list?" 内嵌编号（语义待确认）":" 技能获取配置键"),4,list?ev:lookupEv,!list);
                r.u(p+" 状态 A（语义待确认）",1,ev,false);r.u(p+" 配置次级键（枚举待确认）",1,lookupEv,false);
                if(entryFlags&1)for(int j=0;j<5;++j)r.u(p+" 子状态 "+std::to_string(j),1,"IDA 0x855E690",false);
            }
            r.u("消息附加状态（语义待确认）",1,ev,false);break;
        }
        case 0x3900: {
            const std::string ev="IDA 0x9271430 → 0x92D1910；0x8EF1720 按父技能和槽位更新；SpecializedSkillLoad_NT 0xEDFBB80 / SpecializedSkillInfo 0xEE1E218 / SlotInfo 0xEE1E1E0";
            m.name+=" 技能特化列表";auto n=r.count("父技能数量",ev,5);
            for(uint32_t i=0;i<n;++i){auto p="特化技能 "+std::to_string(i);
                r.u(p+" 父技能 ID",4,ev+"；_parent_skill_id；0x9271535 第二参数");
                auto slots=r.count(p+" 槽位数量",ev,5);
                for(uint32_t j=0;j<slots;++j){auto slotName=p+" 槽位 "+std::to_string(j);
                    auto type=r.u(slotName+" 类型",1,ev+"；_slot_type / ESpecializedSkillSlotType 0xEE2F9F0，getter 0x880D1E0");
                    const char* names[]={"None","Slot_1","Slot_2","Slot_3","Slot_4","Slot_5","Max（枚举边界）"};
                    m.fields.back().value+=" / "+std::string(type<7?names[type]:"未知枚举值");
                    if(type>=6)m.fields.back().meaningKnown=false;
                    r.u(slotName+" 部件 ID（0=清除槽位）",4,ev+"；_parts_id；0x8EF1780 非零更新，0x8EF1CF5 零值删除；父技能或槽位为 0 时消费者忽略");
                }
            }break;
        }
        case 0x5100: {
            const std::string ev="IDA 0x9272BB0 → 0x92D4360 / 0x9056860；MySkillList_NT 0xEDFC7C0 / Common_Skill 0xEE1E090";
            const std::string extraEv=ev+"；0x855E690 / SkillAdditionalLevel 0xEE1E058，getter 0x8790070";
            m.name+=" 自身技能列表";auto n=r.count("技能条目数量",ev,11);
            for(uint32_t i=0;i<n;++i){auto p="技能 "+std::to_string(i);
                auto flags=r.u(p+" 可选字段位图",1,ev);r.flagsKnown(flags,15);
                r.u(p+" 技能 ID",4,ev+"；原生 +0x08；0x90569E3 用作技能键");
                r.u(p+" 等级",1,ev+"；_skill_level / 原生 +0x0C");
                r.u(p+" 原始等级",1,ev+"；_original_skill_level / 原生 +0x0D");
                if(flags&1)for(auto source:{"daevanion","abnormal","equip","arcana","fix"})
                    r.u(p+" 附加等级 "+source,1,extraEv+"；反射成员名保留，缺省为 0");
                if(flags&2)r.var(p+" 冷却时间（毫秒）",ev+"；_cooltime / +0x20；0x905732F → 0x8EDEFD0 乘 10000 加当前日期 tick；0x3FA8FF0 证明每毫秒 10000 tick；缺省 0");
                r.u(p+" 自动使用冷却值（单位待确认）",4,ev+"；_auto_using_cooltime / +0x24；每条必读",false);
                if(flags&4)r.u(p+" 自动装填数量",1,ev+"；_autoload_count / +0x28；0x9057347 → 0x8EDF310；缺省 0");
                if(flags&8)r.var(p+" 自动装填时间（毫秒）",ev+"；_autoload_time / +0x2C；0x9057347 → 0x8EDF310 → 0x8BCBBB0 乘 10000 加当前日期 tick；0x3FA8FF0；缺省 0");
            }break;
        }
        case 0x3806: {
            const std::string ev="IDA 0x926E040 / SkillEnd_NT 0xEDFA808；0x926E131 按实体键查找，0x926E150 分别传递技能 ID / 动作 UID / 原因";
            m.name+=" 技能结束通知";r.var("实体编号",ev);r.u("技能 ID",4,ev);
            r.u("技能动作 UID",1,ev+"；_skill_action_uid；0x8EFD550 同技能同实体内按 UID 清理实例");
            r.u("技能结束原因（枚举待确认）",1,ev+"；_stop_reason，样本 0 不据此命名为正常结束",false);break;
        }
        case 0x3802: {
            const std::string ev="运行时代码 / IDA 0x92CC3A0；字段序列对应 SkillStart_NT 0xEDFA728，处理函数转储为空，消息映射仍待动态核对";
            const std::string targetEv=ev+"；0x8570070 / SkillTargetInfo 0xEE1F830；getter 0x879B120；Vec3 0xEE1DB18";
            m.name+=" 技能开始通知（反射对应）";
            r.var("对象编号（语义待确认）",ev,false);auto flags=r.u("可选字段位图",1,ev);
            r.u("技能 ID（反射对应）",4,ev+"；_skill_id",false);r.u("技能动作 UID（反射对应）",1,ev+"；_skill_action_uid",false);
            if(flags&1)r.u("客户端技能动作 UID（反射对应）",1,ev+"；_client_skill_action_uid",false);
            auto nested=r.u("目标信息位图",1,targetEv);r.var("目标编号（反射对应）",targetEv+"；_key",false);
            if(nested&1)r.u("目标部位 ID（反射对应）",4,targetEv+"；_parts_id",false);
            r.f32("目标 Yaw（反射对应；单位待确认）",targetEv+"；_yaw kind=10，原生 +0x10",false);
            if(nested&2)for(auto axis:{"X","Y","Z"})r.f32(std::string("目标位置 ")+axis+"（反射对应）",targetEv+"；_location Vec3 float32",false);
            if(nested&~3){stop=true;m.status="内嵌结构出现未定义位";break;}
            r.var("攻击速度（反射对应；比例待确认）",ev+"；_attack_speed",false);
            auto move=r.u("动作移动类型（反射对应）",1,ev+"；_act_move_type / EActMoveType 0xEE2C000",false);
            const char* moveNames[]={"None","Ground","Flight","Swimming","Max（枚举边界）"};
            m.fields.back().value+=" / "+std::string(move<5?moveNames[move]:"未知枚举值");
            if(flags&2)r.var("技能动作 ID（反射对应）",ev+"；_skill_act_id",false);
            r.var("冷却值（反射对应；单位待确认）",ev+"；_cooltime",false);
            if(flags&4)r.u("自动装填数量（反射对应）",1,ev+"；_autoload_count",false);
            if(flags&8)r.var("自动装填时间（反射对应；单位待确认）",ev+"；_autoload_time",false);
            if(flags&16)r.boolean("代理施放技能（反射对应）",ev+"；_proxy_using_skill");
            if(flags&32){auto n=r.count("投射物目标数量（反射对应）",ev,13);for(uint32_t i=0;i<n;++i){auto entryName="投射物目标 "+std::to_string(i);r.var(entryName+" 编号（反射对应）",ev+"；ProjectileTargetInfo 0xEE1F868 _key",false);for(auto axis:{"X","Y","Z"})r.f32(entryName+" "+axis+"（反射对应）",ev+"；_pos Vec3 0xEE1DB18",false);}}
            if(flags&~63){stop=true;m.status="出现未定义的可选字段位";}break;
        }
        case 0x3801: {
            const std::string ev="分发 0x92652EB / 运行时 0x926D8E0 → 0x8E46BC0；SkillStart_RS 0xEDFA6F0";
            m.name+=" 技能开始响应";auto flags=r.u("可选字段位图",1,ev);r.flagsKnown(flags,3);
            r.u("结果码（0=成功；其他值待确认）",2,ev+"；_result，线上 u16；消费者以 0 判断成功",false);
            r.u("技能 ID",4,ev+"；_skill_id");
            if(flags&1){
                auto toggle=r.u("技能开关变化",1,ev+"；_change_toggle / ESkillChangeToggle 0xEE2E670",false);
                const char* names[]={"None","On","Off","Max（枚举边界）"};
                r.out.fields.back().value+=" / "+std::string(toggle<4?names[toggle]:"未知枚举值");
                r.out.fields.back().meaningKnown=toggle<3;
            }
            if(flags&2)r.u("客户端技能动作 UID",1,ev+"；_client_skill_action_uid");
            break;
        }
        case 0x3805: {
            const std::string ev="分发 0x9265372 → 运行时 0x926DDC0（0x3805）→ 0x92CD960；SkillEffectByAbnormal_NT 0xEDFA7D0";
            m.name+=" 异常状态引发的技能效果";
            r.var("实体编号",ev+"；继承实体头 +0x0C，0x926DECD 按此键查找接收效果的实体");
            auto flags=r.u("可选字段位图",1,ev);r.flagsKnown(flags,127);
            r.var("施放者实体编号",ev+"；_caster_key");
            r.var("异常状态 UID",ev+"；_abnormal_uid");
            r.u("效果 ID",4,ev+"；_effect_id");
            if(flags&1)r.var64("剩余伤害（具体用途待确认）",ev+"；_remain_dmg",false);
            if(flags&2)r.var64("伤害值",ev+"；_dmg；不等同当前 HP，不据此推算 HP",true);
            if(flags&4){auto n=r.count("屏障 ID 数量",ev,4);for(uint32_t i=0;i<n;++i)r.u("屏障 ID["+std::to_string(i)+"]",4,ev+"；_barrier_id_list");}
            if(flags&8)r.u("技能 ID",4,ev+"；_skill_id");
            if(flags&16)r.var("被免疫阻挡的施放者编号",ev+"；_blocked_skill_caster_key_by_immune");
            if(flags&32)r.u("被免疫阻挡的技能 ID",4,ev+"；_blocked_skill_id_by_immune");
            if(flags&64)r.u("反射技能 ID",4,ev+"；_reflection_skill_id");
            break;
        }
        case 0x3804: {
            const std::string ev="运行时代码 / IDA 0x92CD2A0";m.name+=" 复合状态（用途待确认）";
            r.var("对象编号（语义待确认）",ev,false);auto flags=r.u("可选字段位图",2,ev);
            r.var("对象 +0x14（语义待确认）",ev,false);r.u("对象 +0x18（语义待确认）",4,ev,false);r.u("对象 +0x1C（语义待确认）",1,ev,false);
            if(flags&1)r.u("对象 +0x20（语义待确认）",4,ev,false);
            r.u("对象 +0x24（语义待确认）",1,ev,false);
            if(flags&2){for(int i=0;i<6;++i)r.boolean("内嵌标志 "+std::to_string(i),"IDA 0x8581ED0");r.var64("内嵌 +0x10（语义待确认）","IDA 0x8581ED0 / 0x9330F00");r.boolean("内嵌标志 6","IDA 0x8581ED0");r.u("内嵌 +0x19（语义待确认）",1,"IDA 0x8581ED0",false);}
            r.u("对象 +0x48（语义待确认）",4,ev,false);r.u("对象 +0x4C（语义待确认）",4,ev,false);r.var("对象 +0x50（语义待确认）",ev,false);
            if(flags&4)r.var64("对象 +0x58（语义待确认）",ev);
            if(flags&8)r.u("对象 +0x60（语义待确认）",4,ev,false);
            if(flags&16)r.boolean("对象 +0x64（语义待确认）",ev);
            if(flags&32){auto n=r.count("数组 A 数量",ev,1);for(uint32_t i=0;i<n;++i)r.var64("数组 A["+std::to_string(i)+"]",ev);}
            if(flags&64){auto n=r.count("数组 B 数量",ev,4);for(uint32_t i=0;i<n;++i)r.u("数组 B["+std::to_string(i)+"]",4,ev,false);}
            if(flags&128)r.u("对象 +0x88（语义待确认）",1,ev,false);
            if(flags&256){auto n=r.count("数组 C 数量",ev,4);for(uint32_t i=0;i<n;++i)r.u("数组 C["+std::to_string(i)+"]",4,ev,false);}
            r.u("对象 +0xA0（语义待确认）",2,ev,false);
            if(flags&512)r.var64("对象 +0xA8（语义待确认）",ev);
            if(flags&1024)r.var64("对象 +0xB0（语义待确认）",ev);
            if(flags&2048)r.u("对象 +0xB8（语义待确认）",4,ev,false);
            if(flags&4096)r.boolean("对象 +0xBC（语义待确认）",ev);
            if(flags&8192)r.u("对象 +0xC0（语义待确认）",4,ev,false);
            if(flags&~16383){stop=true;m.status="出现未定义的可选字段位";}break;
        }
        case 0x382B: {
            const std::string ev="IDA 0x855E240 / 0x926F560";m.name+=" 实体附加状态";
            r.var("实体编号","IDA 0x926F5DB / 0x9055ACB");effectFields(r,"内嵌状态");
            r.boolean("显示消息",ev+"；AbnormalChange_NT 0xEDFAFE8 / 属性 0xDBF5190 _show_msg；赋值 0x46E4390",true);break;
        }
        case 0x9003:
            r.var("实体编号","IDA 0x9290A50 / 0x9290AD3");r.u("附加值（语义待确认）",4,"IDA 0x9290A62",false);
            r.u("状态 A（语义待确认）",1,"IDA 0x9290A74",false);r.u("状态 B（语义待确认）",1,"IDA 0x9290A86",false);r.boolean("附加标志（语义待确认）","IDA 0x9290A92");break;
        case 0x9006:
            r.var("实体编号","IDA 0x9290D44 / 0x9290DA7");r.u("状态（语义待确认）",1,"IDA 0x9290D56",false);r.boolean("实体 +0x2D1C 标志（语义待确认）","IDA 0x9290D62 / 0x9290DED");break;
        case 0x8D46: {
            r.u("附加值（语义待确认）",4,"IDA 0x92FEE7D",false);auto n=r.count("字符串数量","IDA 0x92FEE92",1);
            for(uint32_t i=0;i<n;++i)r.stringBytes("字符串 "+std::to_string(i),"IDA 0x92FEED4 / 0x9330FD0");break;
        }
        case 0x8A07:
        case 0x8A09:
        case 0x8A1D: {
            const std::string ev=opcode!=0x8A1D?"2026-09-30 GuildInfo reader 0x14920E880；分发 0x1491A5780 / 0x1491A58C0":"IDA 0x9283250 → 0x92F01C0；GuildInfo 0xEE1E4B8 / getter 0x8792250；列表具体用途待确认";
            m.name+=opcode==0x8A07?" 推荐军团列表响应":opcode==0x8A09?" 军团搜索响应":" 公会信息列表响应";
            r.u("结果码（含义待确认）",2,ev,false);
            auto n=r.count("公会数量",ev,34);
            for(uint32_t i=0;i<n;++i){auto p="公会["+std::to_string(i)+"]";
                r.u(p+" 公会 ID",8,ev+"；_guild_id，不是场景实体编号");
                r.stringBytes(p+" 公会名称",ev+"；_guild_name",true);
                r.stringBytes(p+" 会长名称",ev+"；_master_name",true);
                r.u(p+" 公会等级",1,ev+"；_guild_level");
                r.u(p+" 公会经验",8,ev+"；_exp");
                r.u(p+" 成员数",1,ev+"；_member_count");
                r.u(p+" 成员上限",1,ev+"；_max_member_count");
                r.u(p+" 徽章 ID",2,ev+"；_emblem_id；原生 +0x42 为 u16，镜像 +0x44 为 u32");
                r.stringBytes(p+" 简介",ev+"；_intro",true);
                static constexpr const char* rules[]={"AutoJoin","AutoDeny","Manual","Password"};
                auto rule=r.u(p+" 入会规则",1,ev+"；EGuildJoinRule 0xEE2CDB0",false);
                if(rule<std::size(rules)){r.out.fields.back().value+=" / "+std::string(rules[rule]);r.out.fields.back().meaningKnown=true;}
                r.boolean(p+" 已申请加入",ev+"；_is_join_requested；0x5B9C610 设置 +0x59；bool 位游标跨条目共享",true);
                r.u(p+" 申请时间原值（单位待确认）",8,ev+"；_requested_time",false);
                static constexpr const char* races[]={"None","Light","Dark","All"};
                auto race=r.u(p+" 公会种族",1,ev+"；ERace 0xEE2B970",false);
                if(race<std::size(races)){r.out.fields.back().value+=" / "+std::string(races[race]);r.out.fields.back().meaningKnown=true;}
            }break;
        }
        case 0x8D57: {
            const std::string ev="IDA 0x928C380 / 0x8570650；RetrieveMyLatestSeasonRanking_RS 0xEE04D68 / SeasonRankingData 0xEE1F9F0；消费者 0x90A09E0";
            m.name+=" 本人最近赛季排名响应";
            r.u("排名查询结果（0=成功；其他值待确认）",2,ev+"；_result，非零仍完整读取正文",false);
            static constexpr const char* types[]={"None","Abyss","SeasonMission","BossChallenge","PartyChallenge","ArenaSingle","ArenaTeam","ArenaStrategy","BattlePass","Conqueror","BattlePass1","BattlePass2","BattlePass3","BattlePass4","BattlePass5","BattlePass6","BattlePass7","BattlePass8","BattlePass9","BattlePass10","Suppression","Awaken","Attendance1","Attendance2","Attendance3","Attendance4","Attendance5","ItemSuccession","Onboarding","Onboarding1","Onboarding2","Onboarding3","Onboarding4","Onboarding5","Onboarding6","Onboarding7","Onboarding8","Onboarding9","Onboarding10","EventMission1","EventMission2","EventMission3","EventMission4","EventMission5","EventMission6","EventMission7","EventMission8","EventMission9","EventMission10"};
            auto type=r.u("排名玩法类型",1,ev+"；ESeasonContentsType 0xEE2F3F0",false);
            if(type<std::size(types)){r.out.fields.back().value+=" / "+std::string(types[type]);r.out.fields.back().meaningKnown=true;}
            auto characterClass=[&](const char* label){
                static constexpr const char* names[]={"None","Novice","Gladiator","Templar","Ranger","Assassin","Elementalist","Sorcerer","Cleric","Chanter","Tutorial","Test","Fighter"};
                auto c=r.u(label,1,ev+"；ECharacterClass 0xEE2C690",false);
                if(c<std::size(names)){r.out.fields.back().value+=" / "+std::string(names[c]);r.out.fields.back().meaningKnown=true;}
            };
            characterClass("查询职业类别");
            r.u("排名条目 名次",4,ev+"；_rank");
            r.u("排名条目 角色数据库 ID",8,ev+"；_char_dbid，不是周围对象实体键");
            r.stringBytes("排名条目 昵称",ev+"；_nickname",true);
            r.stringBytes("排名条目 公会名称",ev+"；_guild_name",true);
            r.u("排名条目 公会徽章 ID",2,ev+"；_guild_emblem_id，线上 u16");
            r.u("排名条目 等级",4,ev+"；_level");
            r.u("排名条目 征服者等级",4,ev+"；_conqueror_level");
            characterClass("排名条目 职业类别");
            r.u("排名条目 积分",8,ev+"；_point");
            r.stringBytes("排名条目 额外展示数据",ev+"；_extra_display_data，内部格式待确认",false);
            r.u("排名条目 名次变化原值（符号语义待确认）",4,ev+"；_rank_change",false);
            r.u("排名条目 上赛季名次",4,ev+"；_prev_season_rank");
            r.u("排名条目 段位原值（映射待确认）",4,ev+"；_grade",false);
            r.u("排名条目 展示段位原值（映射待确认）",4,ev+"；_grade_display",false);
            r.u("总排名人数",4,ev+"；_total_rank_count");
            r.u("下次更新时间（Unix 毫秒；0 未设置）",8,ev+"；_next_update_time；0x90A0A1C 从原生 +0x80 读取，0x90A0A8F 乘 10000 加 621355968000000000 日历 tick 基准");
            break;
        }
        case 0x8D91:
            m.name+=" 实体等级更新";
            r.var("实体编号","IDA 0x928F523 / 0x9078188 → 0x94B0020；按实体键查找角色或待创建记录");
            r.u("等级",4,"IDA 0x928F536 → 0x9078140 → 0x93EB8A0；写角色 +0x26C8 组件的 +0x20；待创建记录 +0x5A0 与 0x90373F6 的 CharAppear._level 来源一致；不是 +0x5A4 征服者等级");
            break;
        case 0x8D72:
            r.var("实体编号","IDA 0x928E350");r.var("附加编号（语义待确认）","IDA 0x928E35D",false);r.u("状态（语义待确认）",1,"IDA 0x928E370",false);break;
        case 0x8D74:
            r.var("实体编号","IDA 0x928E4E5");r.u("状态码（语义待确认）",2,"IDA 0x928E4F8",false);
            r.var("附加编号（语义待确认）","IDA 0x928E505",false);r.u("状态（语义待确认）",1,"IDA 0x928E51A",false);break;
        case 0x363A:
            r.var("实体编号","IDA 0x9262753");r.var("关联实体编号","IDA 0x9262762 / 0x907095B -> 实体查找");
            r.u("值 A（语义待确认）",4,"IDA 0x9262777",false);r.u("值 B（语义待确认）",4,"IDA 0x926278C",false);break;
        case 0x3642: {
            r.var("实体编号","IDA 0x92BFE77");auto flags=r.u("可选字段位图",1,"IDA 0x92BFE8F");
            r.u("状态值（语义待确认）",1,"IDA 0x92BFEA1",false);
            if(flags&1)r.u("附加值（语义待确认）",4,"IDA 0x92BFEC4",false);
            if(flags&~1){stop=true;m.status="出现未定义的可选字段位";}break;
        }
        case 0x364E: {
            const std::string ev="IDA 0x926314D / 0x92C3470；CharRotate_NT 0xEDF9298";
            m.name+=" 实体转向通知";
            r.var("实体编号",ev+"；0x92631A4 按实体键查找");
            r.f32("目标朝向（度）",ev+"；_target_dir；0x93B2D10 → 0x66BA230 使用 360 和 pi/360 转为旋转四元数");
            r.f32("上报当前朝向（单位待确认）",ev+"；_current_dir；该接收分支未使用此字段",false);
            auto reason=r.u("转向原因",1,ev+"；ERotateReason 0xEE2E190");
            static constexpr const char* names[]={"None","BT","Abnormal","Skill"};
            if(reason<4)r.out.fields.back().value+=" / "+std::string(names[reason]);
            else {r.out.fields.back().value+=reason==4?" / Max（枚举边界）":" / 未知";r.out.fields.back().meaningKnown=false;}
            break;
        }
        case 0x8D02:case 0x8D03: {
            const bool sprint=opcode==0x8D03;
            const std::string ev=sprint?"IDA 0x9289340 → 0x8E35460；SprintCooltimeUpdate_NT 0xEE03B08":"IDA 0x92892B0 → 0x8E37650；GlideCooltimeUpdate_NT 0xEE03AD0";
            m.name+=sprint?" 自身冲刺冷却更新":" 自身滑翔冷却更新";
            r.var("实体编号",ev+"；继承实体头；处理器未按此键查找，直接更新自身控制器");
            r.u(sprint?"冲刺冷却（毫秒）":"滑翔冷却（毫秒）",4,ev+"；_cooltime：u32 转 float 后乘 10000，加 0x91DDE40 日期 tick；0x3FA8FF0 证明每毫秒 10000 tick");
            break;
        }
        case 0x8D05:r.var("实体编号（关联样本；语义待确认）","IDA 0x92894DD",false);break;
        case 0x8D6D:r.var("实体编号","IDA 0x928DBD8 -> 0x94B0650");break;
        case 0xFFFF: {
            m.name+=" LZ4 压缩封装";auto expected=r.u("解压后字节数",4,"IDA 0x9204F69");
            m.expanded=decompressLz4Block(bytes.subspan(r.pos),size_t(expected));auto inner=splitGameFrames(m.expanded);
            r.add(r.pos,bytes.size()-r.pos,"LZ4 数据块","bytes",std::to_string(m.expanded.size())+" 字节 / "+std::to_string(inner.frames.size())+" 内层帧","IDA 0x3E632F0 / LZ4 Block Format",true);r.pos=bytes.size();
            if(inner.consumed!=m.expanded.size()){stop=true;m.status="解压成功，内层存在不完整帧";}
            break;
        }
        default:supported=false;m.status="正文结构尚未确认";break;
        }
        }
    }catch(const std::exception& e){stop=true;m.status=e.what();m.skins.clear();m.skinsComplete=false;}
    if(m.queryResult && *m.queryResult!=0)m.status="查询失败；"+gameQueryResultText(*m.queryResult)+"。响应附带的角色默认值不代表有效资料。";
    m.parsedBytes=r.pos;m.structureComplete=supported && !stop && r.pos==bytes.size();
    if(r.pos<bytes.size() && m.fields.size()<maxAnalysisFields)r.add(r.pos,bytes.size()-r.pos,"未解析字节","bytes",hex(bytes.subspan(r.pos).first(std::min<size_t>(32,bytes.size()-r.pos))),"保留原始数据；尚无字段结论",false);
    if(m.status.empty())m.status=m.structureComplete?"结构已完整读取；语义未知项另行标注":"存在未解析尾部";
    return m;
}
}
