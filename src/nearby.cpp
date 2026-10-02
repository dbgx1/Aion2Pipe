#include "nearby.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
namespace aion {
const char* objectKindName(ObjectKind k){switch(k){case ObjectKind::Player:return "玩家";case ObjectKind::Npc:return "NPC/怪物";case ObjectKind::Environment:return "环境对象";case ObjectKind::Mimic:return "拟态对象";default:return "类型待确认";}}
static const GameField* field(const GameMessage& m,const std::string& name){for(const auto& f:m.fields)if(f.name==name)return &f;return nullptr;}
static std::optional<uint64_t> integer(const GameField* f){if(!f)return {};uint64_t v{};auto end=f->value.data()+f->value.size();auto result=std::from_chars(f->value.data(),end,v);if(result.ec!=std::errc{})return {};return v;}
static void clearLocalMovement(NearbyObject& o){
    for(const auto& name:{"local_vessel_key","local_x","local_y","local_z","local_delta_x","local_delta_y","local_delta_z","local_yaw"})o.values.erase(name);
}
bool NearbyObjects::outbound(std::span<const uint8_t> bytes,uint64_t packet,uint64_t timeUs){
    auto m=decodeGameFrame(bytes,true,true);auto op=integer(field(m,"消息编号"));
    if(!op || *op<0x3700 || *op>0x3703 || !m.structureComplete)return false;
    std::array<double,3> position{};
    for(size_t i=0;i<3;++i){auto f=field(m,std::string(1,"XYZ"[i]));
        if(!f || !f->meaningKnown || f->type!="float32 LE" || f->offset+4>bytes.size())return false;
        position[i]=std::bit_cast<float>(uint32_t(readInteger(bytes,f->offset,4,false)));
        if(!std::isfinite(position[i]))return false;
    }
    NearbyObject* self=nullptr;
    for(auto& [key,o]:objects_)if(o.isSelf && o.present){if(self)return false;self=&o;}
    // Importing a historical state must not rewind an appearance, scene or a newer RX position.
    if(!self || packet<self->firstPacket || packet<self->positionPacket)return false;
    self->position=position;self->positionNeedsRefresh=false;self->positionSource="client_report";self->positionPacket=packet;
    clearLocalMovement(*self);
    if(auto f=field(m,"客户端时间（时基待确认）"))self->values["position_client_time"]=f->value;
    if(packet>=self->lastPacket){self->lastPacket=packet;self->timeUs=timeUs;self->lastMessage=m.name;}
    ++updates;return true;
}
void SelfMovementStream::feed(const Stream& s,const Endpoint& source,const Endpoint& destination,
        const CipherSnapshot& snapshot,std::span<const StreamObservation> observations,NearbyObjects& model){
    if(source!=snapshot.source || destination!=snapshot.destination){status="会话状态与发送连接不匹配";return;}
    if(!state_){
        const auto base=s.initialSequence+(s.startedWithSyn?1u:0u);const auto offset=int32_t(snapshot.frameSequence-base);
        if(offset<0 || size_t(offset)>=s.bytes.size()){status="等待会话锚点，无法跳过缺失数据";return;}
        consumed_=size_t(offset);state_=snapshot;
    }
    if(consumed_>s.bytes.size()){status="流已重置，需要重新匹配会话";return;}
    auto tail=std::span(s.bytes).subspan(consumed_);auto split=splitGameFrames(tail);
    if(!split.frames.empty() && (observations.empty() || observations.back().end<consumed_+split.consumed)){status="缺少帧到达时间记录";return;}
    for(const auto& f:split.frames){
        auto end=consumed_+f.offset+f.length;
        auto observation=std::lower_bound(observations.begin(),observations.end(),end,[](const StreamObservation& o,size_t n){return o.end<n;});
        if(observation==observations.end()){status="缺少帧到达时间记录";return;}
        Bytes plain(tail.begin()+f.offset,tail.begin()+f.offset+f.length);
        state_->transform(std::span(plain).subspan(f.prefixBytes));++frames;
        auto m=decodeGameFrame(plain,true,true);if(!m.structureComplete)++incomplete;
        if(model.outbound(plain,observation->packet,observation->timeUs))++updates;
    }
    consumed_+=split.consumed;status=s.hasGap()?"发送流存在缺口，等待补齐":split.status;
}
void NearbyObjects::feed(const Stream& s,uint64_t packet,uint64_t timeUs,size_t available){
    gap=s.hasGap();midstream=!s.startedWithSyn;closed=s.closed;limited=limited||s.limited;
    available=std::min(available,s.bytes.size());if(consumed>available)return;
    auto tail=std::span(s.bytes).first(available).subspan(consumed);auto frames=splitGameFrames(tail);
    for(const auto& f:frames.frames)message(tail.subspan(f.offset,f.length),packet,timeUs);
    consumed+=frames.consumed;framingStatus=frames.status;
}
void NearbyObjects::message(std::span<const uint8_t> bytes,uint64_t packet,uint64_t timeUs,size_t depth){
    auto invalidatePositions=[&](){for(auto& [key,o]:objects_){o.positionNeedsRefresh=true;o.networkPositionNeedsRefresh=true;}};
    if(depth>4){++unparsed;invalidatePositions();return;}
    auto m=decodeGameFrame(bytes,true);++decoded;
    auto op=integer(field(m,"消息编号"));if(!op){++unparsed;lastUnparsedOpcode=0;lastUnparsedStatus=m.status;return;}
    if(!m.structureComplete){lastUnparsedOpcode=uint32_t(*op);lastUnparsedStatus=m.status;}
    if(*op==0xFFFF){
        if(!m.structureComplete){++unparsed;invalidatePositions();return;}
        auto frames=splitGameFrames(m.expanded);
        for(const auto& f:frames.frames)message(std::span(m.expanded).subspan(f.offset,f.length),packet,timeUs,depth+1);
        return;
    }
    const bool appear=*op==0x3633 || *op==0x3634 || *op==0x3641 || *op==0x3645;
    const bool remove=*op==0x3635 || *op==0x3642 || *op==0x3647;
    const bool correction=*op>=0x373E && *op<=0x3740;
    const bool attackMove=*op==0x372E || *op==0x372F;
    const bool absolute=(*op>=0x371A && *op<=0x371C) || *op==0x371E || *op==0x371F || *op==0x3729 || correction || attackMove;
    const bool delta=*op==0x371D || *op==0x3720 || *op==0x372A;
    if(!m.structureComplete){
        ++unparsed;
        // Even a partially decoded coordinate update can invalidate our baseline.
        // Keep the old value for inspection, but never compound a later delta on it.
        if(appear || absolute || delta){
            auto id=integer(field(m,"实体编号"));
            if(id && *id<=UINT32_MAX){auto it=objects_.find(uint32_t(*id));if(it!=objects_.end()){it->second.positionNeedsRefresh=true;it->second.networkPositionNeedsRefresh=true;}}
            else invalidatePositions();
        }
        return;
    }
    if(*op==0x3621){objects_.clear();++scene;return;}
    // Sprint responses act on native self, not the inherited entity key.
    // They carry no position and must not manufacture an object or self identity.
    if(*op==0x3741 || *op==0x3742)return;
    if(*op==0x8D02 || *op==0x8D03){
        // The native handlers ignore the inherited key and update the local
        // controller. Require an unambiguous, already observed self identity.
        NearbyObject* self=nullptr;
        for(auto& [key,o]:objects_)if(o.isSelf && o.present){if(self)return;self=&o;}
        const bool sprint=*op==0x8D03;
        auto cooldown=field(m,sprint?"冲刺冷却（毫秒）":"滑翔冷却（毫秒）");
        if(self && cooldown && packet>=self->lastPacket){
            self->values[sprint?"sprint_cooldown_ms":"glide_cooldown_ms"]=cooldown->value;
            self->lastPacket=packet;self->timeUs=timeUs;self->lastMessage=m.name;++updates;
        }
        return;
    }
    if(*op==0x3746){
        // The native consumer updates the local movement component, not an
        // entity lookup. Do not infer self identity or modify current XYZ.
        auto z=field(m,"最近地面高度 Z");
        if(z)for(auto& [key,o]:objects_)if(o.isSelf && o.present){
            o.values["last_on_ground_z"]=z->value;o.lastPacket=packet;o.timeUs=timeUs;o.lastMessage=m.name;++updates;break;
        }
        return;
    }
    const auto entityField=field(m,"实体编号");
    const bool update=entityField && entityField->meaningKnown;
    if(!appear && !remove && !update)return;
    auto id=integer(field(m,"实体编号"));if(!id || *id>UINT32_MAX)return;
    auto key=uint32_t(*id);auto it=objects_.find(key);
    if(it==objects_.end()){
        if(objects_.size()>=10000){limited=true;return;}
        it=objects_.emplace(key,NearbyObject{}).first;it->second.key=key;it->second.firstPacket=packet;
    }
    auto& o=it->second;
    if((*op==0x364E || *op==0x8D91) && packet<o.lastPacket)return;
    if(appear){o=NearbyObject{};o.key=key;o.firstPacket=packet;o.appearanceSeen=true;
        o.skins=m.skins;
        o.isSelf=*op==0x3633;
        o.kind=(*op==0x3633 || *op==0x3645)?ObjectKind::Player:*op==0x3641?ObjectKind::Npc:ObjectKind::Environment;
        if(*op==0x3645 && integer(field(m,"拟态 NPC 模板 ID")).value_or(0))o.kind=ObjectKind::Mimic;
    }
    o.lastPacket=packet;o.timeUs=timeUs;o.lastMessage=m.name;++updates;
    if(remove){o.present=false;if(o.kind==ObjectKind::Unknown)o.kind=*op==0x3635?ObjectKind::Environment:*op==0x3642?ObjectKind::Npc:ObjectKind::Player;return;}
    // Updates to a departed key do not revive its old identity; await appearance.
    if(!o.present)return;
    auto copy=[&](const char* from,const char* to){if(auto f=field(m,from))o.values[to]=f->value;};
    if(appear){
        if(o.isSelf){copy("最近地面位置 Z","last_on_ground_z");copy("滑翔冷却（毫秒）","glide_cooldown_ms");copy("冲刺冷却（毫秒）","sprint_cooldown_ms");}
        for(const auto& pair:std::initializer_list<std::pair<const char*,const char*>>{
            {"基础结构 角色昵称","name"},{"基础结构 PC ID","template_id"},{"基础结构 种族","race"},{"基础结构 性别","gender"},
            {"等级","level"},{"征服者等级","conqueror_level"},{"装备等级","equipment_level"},{"服务器 ID","server_id"},
            {"角色数据库 ID","character_dbid"},{"战斗力","combat_power"},{"拟态 NPC 模板 ID","mimic_npc_id"},
            {"地图实例键","map_key"},{"地图 ID","map_id"},{"子区域 ID","subzone_id"},{"频道编号","channel"},
            {"状态结构 HP","hp"},{"状态结构 HP 上限","max_hp"},{"状态结构 MP","mp"},{"状态结构 MP 上限","max_mp"},
            {"状态结构 翅膀 ID","wing_id"},{"状态结构 翅膀等级","wing_level"},{"状态结构 翅膀外观 ID","wing_skin_id"},{"状态结构 称号 ID","title_id"},
            {"关联结构 公会名称","guild"},{"关联结构 公会编号","guild_id"},{"关联结构 联盟名称","union_name"}})copy(pair.first,pair.second);
    }
    if(*op==0x8D91)copy("等级","level");
    if(*op==0x364E){
        // This notification changes rotation only. Neither field is XYZ, and
        // the reported current direction is not a new authoritative position.
        for(const auto& pair:std::initializer_list<std::pair<const char*,const char*>>{
                {"目标朝向（度）","rotate_target_degrees"},{"上报当前朝向（单位待确认）","rotate_reported_current"}}){
            if(auto f=field(m,pair.first);f && f->offset+4<=bytes.size()){
                auto angle=std::bit_cast<float>(uint32_t(readInteger(bytes,f->offset,4,false)));
                if(std::isfinite(angle))o.values[pair.second]=f->value;
            }
        }
        copy("转向原因","rotate_reason");
    }
    if(*op==0x8A33){copy("公会结构 公会名称","guild");copy("公会结构 公会编号","guild_id");copy("公会结构 联盟名称","union_name");}
    if(*op==0x382A)copy("状态条目数量","abnormal_count");
    if(*op==0x8D00){
        for(const auto& f:m.fields)for(const auto& pair:std::initializer_list<std::pair<const char*,const char*>>{{" HP 数值","hp"},{" MaxHP 数值","max_hp"},{" MP 数值","mp"},{" MaxMP 数值","max_mp"}})
            if(f.meaningKnown && f.name.ends_with(pair.first))o.values[pair.second]=f.value;
    }
    if(absolute || delta){
        const auto vessel=integer(field(m,"局部载体实体编号")).value_or(0);
        if(vessel){
            // Native consumers may transform local position by a referenced
            // vessel (94149E0). We lack that transform, so expose the local
            // data without claiming either vector is current world position.
            o.values["local_vessel_key"]=std::to_string(vessel);
            for(const auto& name:{"local_x","local_y","local_z","local_delta_x","local_delta_y","local_delta_z"})o.values.erase(name);
            copy("局部位置 X","local_x");copy("局部位置 Y","local_y");copy("局部位置 Z","local_z");
            copy("局部 delta X","local_delta_x");copy("局部 delta Y","local_delta_y");copy("局部 delta Z","local_delta_z");
            copy("局部朝向 yaw","local_yaw");
            o.positionNeedsRefresh=true;o.networkPositionNeedsRefresh=true;
            return;
        }
        clearLocalMovement(o);
    }
    if(appear || absolute){
        std::array<double,3> p{};bool valid=true;
        for(size_t i=0;i<3;++i){std::string axis(1,"XYZ"[i]);auto name=appear?(*op==0x3634?"位置 "+axis:"状态结构 位置 "+axis+"（单位待确认）"):axis;
            auto f=field(m,name);if(!f || f->type!="float32 LE" || f->offset+4>bytes.size()){valid=false;break;}
            p[i]=std::bit_cast<float>(uint32_t(readInteger(bytes,f->offset,4,false)));if(!std::isfinite(p[i]))valid=false;
        }
        if(valid){o.position=p;o.positionNeedsRefresh=false;o.positionSource=appear?"appearance":correction?"server_correction":attackMove?"server_attack":"absolute";o.positionPacket=packet;
            // Attack consumers 904C470/904CDD0 pass raw XYZ at input+0x60
            // through 93C20E0 -> 93B2B80 to the same network +0x20 baseline
            // used by 90453E0 deltas. Rejection follows a separate path.
            if(correction){o.networkPosition.reset();o.networkPositionNeedsRefresh=true;}
            else{o.networkPosition=p;o.networkPositionNeedsRefresh=false;}
        }
        else{o.position.reset();o.networkPosition.reset();o.networkPositionNeedsRefresh=true;o.positionNeedsRefresh=true;o.positionSource.clear();o.positionPacket=0;}
    }
    if(correction){copy("移动状态","move_state");copy("移动子状态（枚举待确认）","move_substate");copy("拒绝原因（枚举待确认）","move_reject_reason");}
    // 0x90453E0 / 0x904A200 add signed i8 deltas directly to the last NETWORK
    // position (+0x20/+0x28/+0x30), separate from the render interpolation cache.
    // Nonzero local vessel references wait for a world transform above.
    if(delta){
        if(!o.networkPosition || o.networkPositionNeedsRefresh){if(o.positionSource!="client_report")o.positionNeedsRefresh=true;return;}
        auto next=*o.networkPosition;bool valid=true;
        for(size_t i=0;i<3;++i)if(auto f=field(m,std::string("delta ")+"XYZ"[i])){
            if(f->offset>=bytes.size()){valid=false;break;}
            next[i]+=std::bit_cast<int8_t>(bytes[f->offset]);
            valid=valid && std::isfinite(next[i]);
        }
        if(valid){o.networkPosition=next;o.position=next;o.positionNeedsRefresh=false;o.positionSource="delta";o.positionPacket=packet;}
        else {o.positionNeedsRefresh=true;o.networkPositionNeedsRefresh=true;}
    }
}
std::string NearbyObjects::json(bool includeDeparted) const {
    auto rows=nlohmann::json::array();
    for(const auto& [key,o]:objects_){if(!includeDeparted && !o.present)continue;
        nlohmann::json row={{"entity_key",key},{"kind",objectKindName(o.kind)},{"is_self",o.isSelf},{"observed_present",o.present},{"appearance_seen",o.appearanceSeen},
            {"first_packet",o.firstPacket},{"last_packet",o.lastPacket},{"capture_time_us",o.timeUs},{"last_message",o.lastMessage},{"fields",o.values},
            {"position_needs_refresh",o.positionNeedsRefresh},{"position_source",o.positionSource},{"position_packet",o.positionPacket},
            {"position_units","unverified"},{"position",nullptr}};
        if(o.position)row["position"]=*o.position;
        rows.push_back(std::move(row));
    }
    return rows.dump(2,' ',false,nlohmann::json::error_handler_t::replace);
}
}
