#include "ui.hpp"
#include "game_protocol.hpp"
#include "diagnostics.hpp"
#include "world_probe.hpp"
#include <imgui.h>
#include <commdlg.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <charconv>

namespace aion {
static unsigned skinRace(const std::string& text){unsigned n{};std::from_chars(text.data(),text.data()+text.size(),n);return n;}
static const SkinCatalog& skinCatalog(){
    static const SkinCatalog catalog=[](){SkinCatalog c;wchar_t exe[32768]{};auto n=GetModuleFileNameW(nullptr,exe,DWORD(std::size(exe)));
        if(n && n<std::size(exe))c.load(std::filesystem::path(exe).parent_path()/L"skin_catalog.json");return c;}();return catalog;
}
struct SkinUiPreferences {
    SkinPreferences data;std::filesystem::path path;std::string error;
    SkinUiPreferences(){try{path=CharacterReporter::defaultDirectory().parent_path()/L"skin-preferences.json";data.load(path);}catch(const std::exception& e){error=e.what();}}
    void save(){try{if(path.empty())throw std::runtime_error("无法确定本地设置目录");data.save(path);error.clear();}catch(const std::exception& e){error=e.what();}}
    void toggle(uint32_t id){if(!data.favorites.erase(id))data.favorites.insert(id);save();}
};
static SkinUiPreferences& skinPreferences(){static SkinUiPreferences p;return p;}
static std::string skinLabel(const SkinEquipment& s,unsigned race){return skinCatalog().name(s.skinId,race)+" ["+std::to_string(s.skinId)+"]";}
static void skinView(const std::vector<SkinEquipment>& skins,unsigned race){
    if(!ImGui::CollapsingHeader("时装名称与颜色",ImGuiTreeNodeFlags_DefaultOpen))return;
    const auto& catalog=skinCatalog();
    ImGui::TextWrapped("名称表：%s",catalog.status.c_str());
    ImGui::TextDisabled("颜色为网络染色值；默认材质色和光照效果不在 RGB 中。名称缺失时保留 ID。");
    if(skins.empty()){ImGui::TextDisabled("尚无装备外观记录，请查询玩家资料。");return;}
    if(ImGui::BeginTable("skinDetailsFocus",6,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Resizable)){
        for(auto title:{"部位","时装名称","时装 ID","颜色 / 染色槽","显示","重点"})ImGui::TableSetupColumn(title);ImGui::TableHeadersRow();
        for(size_t i=0;i<skins.size();++i){const auto& s=skins[i];if(!s.skinId && s.dyes.empty())continue;
            ImGui::PushID(int(i));ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%s (%u)",visualSlotName(s.visualSlot),s.visualSlot);
            ImGui::TableNextColumn();ImGui::TextWrapped("%s",catalog.name(s.skinId,race).c_str());
            ImGui::TableNextColumn();ImGui::Text("%u",s.skinId);
            ImGui::TableNextColumn();if(s.dyes.empty())ImGui::TextUnformatted("默认色（无染色记录）");
            for(size_t j=0;j<s.dyes.size();++j){const auto& d=s.dyes[j];ImGui::PushID(int(j));
                ImGui::Text("槽 %u",d.slot);ImGui::SameLine();
                auto color=[](const char* label,const std::array<uint8_t,3>& rgb){ImGui::ColorButton(label,ImVec4(rgb[0]/255.f,rgb[1]/255.f,rgb[2]/255.f,1),ImGuiColorEditFlags_NoTooltip,ImVec2(18,18));ImGui::SameLine();ImGui::Text("%s (%u, %u, %u)",skinColorHex(rgb).c_str(),rgb[0],rgb[1],rgb[2]);};
                if(d.remove)ImGui::TextUnformatted("默认色（移除染色）");else color("##dye",d.rgb);
                if(d.patternId){ImGui::Text("图案 %u",d.patternId);ImGui::SameLine();if(d.patternRemove)ImGui::TextUnformatted("图案默认色");else color("##pattern",d.patternRgb);}
                ImGui::PopID();
            }
            ImGui::TableNextColumn();ImGui::TextUnformatted(s.display?"开启":"隐藏");
            if(ImGui::IsItemHovered())ImGui::SetTooltip("装备 ID %u；默认模型标志 %s；覆盖标志 %s。标志组合的渲染优先级尚未确认。",s.itemId,s.defaultMesh?"true":"false",s.overrideMesh?"true":"false");
            ImGui::TableNextColumn();if(s.skinId){auto& pref=skinPreferences();bool focus=pref.data.favorites.contains(s.skinId);if(ImGui::Checkbox("##focus",&focus))pref.toggle(s.skinId);}
            ImGui::PopID();
        }ImGui::EndTable();
    }
}
static std::filesystem::path dialog(bool save,const wchar_t* filter,const wchar_t* extension) {
    wchar_t file[32768]{}; OPENFILENAMEW o{}; o.lStructSize=sizeof(o); o.lpstrFile=file; o.nMaxFile=32768;
    o.lpstrFilter=filter; o.lpstrDefExt=extension; o.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if((save?GetSaveFileNameW(&o):GetOpenFileNameW(&o))) return file;
    return {};
}
static std::wstring wide(const char* text) {
    int n=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0); std::wstring s(size_t(n),0);
    MultiByteToWideChar(CP_UTF8,0,text,-1,s.data(),n); if(!s.empty())s.pop_back(); return s;
}
void applyTheme() {
    ImGui::StyleColorsDark(); auto& s=ImGui::GetStyle();
    s.WindowPadding={18,14}; s.FramePadding={9,6}; s.ItemSpacing={9,8}; s.FrameRounding=5; s.GrabRounding=4;
    s.ChildRounding=6; s.ScrollbarRounding=4; s.WindowBorderSize=0; s.FrameBorderSize=0;
    s.Colors[ImGuiCol_WindowBg]={0.045f,0.06f,0.09f,1};
    s.Colors[ImGuiCol_ChildBg]={0.063f,0.082f,0.12f,1};
    s.Colors[ImGuiCol_FrameBg]={0.10f,0.13f,0.18f,1};
    s.Colors[ImGuiCol_Header]={0.12f,0.27f,0.34f,1};
    s.Colors[ImGuiCol_HeaderHovered]={0.16f,0.34f,0.41f,1};
    s.Colors[ImGuiCol_Button]={0.12f,0.26f,0.34f,1};
    s.Colors[ImGuiCol_ButtonHovered]={0.12f,0.42f,0.49f,1};
    s.Colors[ImGuiCol_CheckMark]={0.25f,0.83f,0.73f,1};
    s.Colors[ImGuiCol_Text]={0.86f,0.91f,0.95f,1};
    s.Colors[ImGuiCol_TableHeaderBg]={0.095f,0.13f,0.19f,1};
}
App::App()=default;
App::~App(){queryWorker_.reset();chatBridge_.stop();add(queryProxy_.takePackets());diagnosticSnapshot(true);diagnostics().write("app_close","Final capture state flushed");}
void App::autoStart(){
    diagnostics().write("autostart","Starting proxy, chat capture, character reporter, and query service");
    chatAutoStart_=true;chatWorldSeen_=false;
    startCapture();
    if(!chatBridge_.start())diagnostics().write("chat_bridge_autostart_error",chatBridge_.status().message);
    try{
        reporter_=std::make_unique<CharacterReporter>(CharacterReporter::defaultDirectory());const auto cfg=reporter_->config();
        strncpy_s(reportUrl_,cfg.url.c_str(),_TRUNCATE);strncpy_s(reportToken_,cfg.token.c_str(),_TRUNCATE);reporter_->configure(cfg,true);
        diagnostics().write("report_autostart",reporter_->status().message);
    }catch(const std::exception& e){diagnostics().write("report_autostart_error",e.what());}
    try{
        queryWorker_=std::make_unique<QueryWorker>(queryProxy_,CharacterReporter::defaultDirectory().parent_path()/L"query-worker");
        const auto cfg=queryWorker_->config();snprintf(queryWorkerId_,sizeof(queryWorkerId_),"%s",cfg.clientId.c_str());queryWorker_->start(cfg);
        diagnostics().write("query_worker_autostart","started=1");
    }catch(const std::exception& e){diagnostics().write("query_worker_autostart_error",e.what());}
}
void App::refreshSelfPositions(){
    if(proxyData_ || !cipherSnapshot_)return;
    for(const auto& [tx,rx]:reverseStreams_){
        auto model=nearby_.find(rx);if(model==nearby_.end())continue;
        const auto& endpoints=streamEndpoints_.at(tx);
        if(endpoints.first!=cipherSnapshot_->source || endpoints.second!=cipherSnapshot_->destination)continue;
        selfMovement_[tx].feed(streams_.at(tx),endpoints.first,endpoints.second,*cipherSnapshot_,streamObservations_[tx],model->second);
    }
}
void App::diagnosticSnapshot(bool force){
    auto now=GetTickCount64();if(!force && now-diagnosticTick_<2000)return;diagnosticTick_=now;
    size_t probes=0;
    for(const auto& [key,stream]:streams_){
        if(key.starts_with("wire/") || key.starts_with("proxy:") || nearby_.contains(key) || stream.bytes.empty() || worldProbeSizes_[key]==stream.bytes.size() || probes>=64)continue;
        ++probes;worldProbeSizes_[key]=stream.bytes.size();
        if(auto start=findWorldStreamStart(stream.bytes)){
            auto& model=nearby_[key];model.consumed=*start;
            for(const auto& observed:streamObservations_[key])if(observed.end>*start)
                model.feed(stream,observed.packet,observed.timeUs,observed.end);
            if(nearbyConnection_.empty() || followNearby_)nearbyConnection_=key;
            diagnostics().write("world_stream_detected","stream="+std::to_string(diagnosticStreamIds_[key])+" method=validated_messages start_offset="+std::to_string(*start));
        }else diagnostics().write("world_stream_probe","stream="+std::to_string(diagnosticStreamIds_[key])+" bytes="+std::to_string(stream.bytes.size())+" result=no_confirmed_plaintext_world_sequence");
    }
    refreshSelfPositions();
    const auto stats=queryProxy_.stats();size_t objects=0,updates=0;
    for(const auto& [key,model]:nearby_){objects+=model.objects().size();updates+=model.updates;}
    std::ostringstream summary;summary<<"mode="<<mode_<<" running="<<queryProxy_.running()<<" target_processes="<<stats.processes<<" proxy_connections="<<stats.connections
        <<" observed_messages="<<stats.received<<" target_matched="<<stats.matched<<" unmatched="<<stats.unmatched<<" unsupported="<<stats.unsupported<<" queue_dropped="<<stats.queueDropped
        <<" observed_records="<<observedRecords_<<" retained_packets="<<packets_.size()<<" tcp_streams="<<streams_.size()<<" world_inbound_streams="<<nearby_.size()<<" objects_including_departed="<<objects<<" object_updates="<<updates
        <<" proxy_self_updates="<<proxySelfUpdates_<<" capacity_limit="<<limit_<<" selected_stream="<<(diagnosticStreamIds_.contains(nearbyConnection_)?diagnosticStreamIds_.at(nearbyConnection_):0)
        <<" follow_latest="<<followNearby_<<" kind_filter="<<nearbyKind_<<" name_filter_active="<<bool(*nearbyFilter_)<<" include_departed="<<showDeparted_;
    diagnostics().write("capture_snapshot",summary.str());
    // Visit world candidates first. Bound diagnostic work on hosts with many connections.
    size_t sampled=0;for(int pass=0;pass<2;++pass)for(const auto& [key,stream]:streams_){
        const bool world=nearby_.contains(key);if(world!=(pass==0) || sampled>=64)continue;++sampled;
        std::ostringstream d;d<<"stream="<<diagnosticStreamIds_[key]<<" bytes="<<stream.bytes.size()<<" pending_bytes="<<stream.pendingBytes<<" gap="<<stream.hasGap()
            <<" started_with_syn="<<stream.startedWithSyn<<" closed="<<stream.closed<<" limited="<<stream.limited<<" duplicate_bytes="<<stream.duplicateBytes;
        if(world){const auto& m=nearby_.at(key);size_t present=0;for(const auto& [id,o]:m.objects())present+=o.present;
            d<<" consumed="<<m.consumed<<" undecoded_bytes="<<(stream.bytes.size()>=m.consumed?stream.bytes.size()-m.consumed:0)<<" decoded_messages="<<m.decoded
                <<" incomplete_messages="<<m.unparsed<<" objects="<<m.objects().size()<<" present="<<present<<" updates="<<m.updates<<" scene="<<m.scene
                <<" framing="<<m.framingStatus<<" last_incomplete_opcode="<<m.lastUnparsedOpcode<<" last_incomplete_status="<<m.lastUnparsedStatus;
            auto tx=reverseStreams_.find(key);
            d<<" legacy_cipher_loaded="<<bool(cipherSnapshot_)<<" proxy_connection="<<(proxyStreamIds_.contains(key)?proxyStreamIds_.at(key):0);
            for(const auto& [id,o]:m.objects())if(o.isSelf && o.present)d<<" self_position_source="<<o.positionSource<<" self_position_record="<<o.positionPacket;
            if(tx!=reverseStreams_.end())if(auto it=selfMovement_.find(tx->second);it!=selfMovement_.end())
                d<<" self_tx_frames="<<it->second.frames<<" self_tx_updates="<<it->second.updates<<" self_tx_incomplete="<<it->second.incomplete<<" self_tx_status="<<it->second.status;
        }
        auto details=d.str();if(force || diagnosticLastStreams_[key]!=details){diagnostics().write("tcp_stream_snapshot",details);diagnosticLastStreams_[key]=std::move(details);}
    }
    if(sampled<streams_.size())diagnostics().write("snapshot_limit","Only first 64 streams sampled; total="+std::to_string(streams_.size()));
}
void App::clear() {
    diagnosticSnapshot(true);diagnostics().write("session_clear","Clearing packets and connection state");diagnosticStreamIds_.clear();diagnosticLastStreams_.clear();
    nearby_.clear();worldProbeSizes_.clear();nearbyConnection_.clear();nearbySelected_.reset();
    selfMovement_.clear();reverseStreams_.clear();streamEndpoints_.clear();streamObservations_.clear();cipherSnapshot_.reset();pendingDecrypt_=false;
    proxyData_=false;proxySelfUpdates_=0;recordedConnections_.clear();proxyStreamIds_.clear();
    expandedGame_.clear();decryptedGame_.clear();gameFrame_=0;
    packets_.clear(); streams_.clear(); generations_.clear(); packetStreams_.clear(); selected_=-1;
    memory_=streamMemory_=0; observedRecords_=0; limit_=false; byteOffset_=0;
}
void App::demo(){if(queryProxy_.running()){message_="请结束游戏连接并关闭代理后再加载示例。";return;} clear(); add(demoPackets()); selected_=1; mode_="DEMO / 合成数据"; message_="示例采用 u16 LE 总长度 + u16 LE 消息号；包含乱序与重传，不是 Aion2 协议定义。";}
void App::open(const std::filesystem::path& path) {
    diagnostics().write("pcap_open","User opened offline capture (filename omitted)");
    if(queryProxy_.running()){message_="请结束游戏连接并关闭代理后再打开离线文件。";return;}
    try {bool incomplete=false;auto data=path.extension()==L".a2session"?loadSession(path,&incomplete):loadPcap(path); clear(); add(std::move(data));limit_|=incomplete; mode_="OFFLINE / 离线";
        for(size_t i=0;i<packets_.size();++i) if(!packets_[i].payload.empty() && !packets_[i].wireObservation) {selected_=int(i);break;}
        message_=proxyData_?"已回放代理会话，收发明文、周围对象与自身位置已恢复，无需密钥。":"已导入历史 PCAP；标准 PCAP 不保存代理明文及方向。";
        diagnosticSnapshot(true);
    } catch(const std::exception& e){message_=e.what();diagnostics().write("pcap_error",message_);}
}
void App::startCapture() {
    if(queryProxy_.running())return;
    if(queryPort_<1 || queryPort_>65535 || upstreamProxyPort_<0 || upstreamProxyPort_>65535){message_="端口必须为 0–65535，世界端口不能为 0";return;}
    diagnostics().write("proxy_start_request",std::string("target=")+process_+" upstream_proxy_port="+std::to_string(upstreamProxyPort_));
    upstreamProxyPort_=0;
    if(queryProxy_.start(wide(process_),uint16_t(queryPort_))) {clear();mode_="LIVE / 独立代理";message_="代理已启用，请重新登录游戏。当前："+queryProxy_.routingMode();}
    else message_=queryProxy_.status();
}
void App::openCipherState(const std::filesystem::path& path) {
    if(proxyData_ || queryProxy_.running()){message_="独立代理自动解密，无需导入会话状态。";return;}
    try {cipherSnapshot_=loadCipherSnapshot(path);selfMovement_.clear();source_=2;gameMode_=2;selectGameTab_=true;pendingDecrypt_=true;
        for(size_t i=0;i<packets_.size();++i)if(packets_[i].source==cipherSnapshot_->source && packets_[i].destination==cipherSnapshot_->destination && !packets_[i].payload.empty()){selected_=int(i);break;}
        refreshSelfPositions();
    }catch(const std::exception& e){message_=e.what();}
}
// Proxy observations already contain ordered, complete application frames. Keep
// current world state independent of the bounded packet/stream archive.
void App::observeProxy(const Packet& p,uint64_t record) {
    if(!p.proxyConnection || p.wireObservation || p.protocol!=6)return;
    Packet inbound;inbound.proxyConnection=p.proxyConnection;inbound.pid=p.pid;inbound.protocol=p.protocol;
    inbound.source=p.outbound?p.destination:p.source;inbound.destination=p.outbound?p.source:p.destination;
    const auto key=inbound.directionKey()+"/0";
    if(inbound.source.port==13700)return;
    proxyStreamIds_[key]=p.proxyConnection;
    auto& model=nearby_[key];
    if(p.flags&5)model.closed=true;
    if(!p.plaintext || p.payload.empty())return;
    if(p.outbound){
        if(!p.toolGenerated && model.outbound(p.payload,record,p.timeUs))++proxySelfUpdates_;
    }else{
        auto frames=splitGameFrames(p.payload);
        for(const auto& f:frames.frames)model.message(std::span(p.payload).subspan(f.offset,f.length),record,p.timeUs);
        model.consumed+=frames.consumed;model.framingStatus=frames.status;
        if(followNearby_ || nearbyConnection_.empty())nearbyConnection_=key;
    }
}
void App::add(std::vector<Packet> incoming) {
    for(auto& p:incoming) {
        const auto record=++observedRecords_;
        proxyData_|=p.proxyConnection!=0;
        observeProxy(p,record);
        if(limit_)continue;
        auto cost=p.raw.size()+p.payload.size();
        if(packets_.size()>=50000 || memory_+cost>128*1024*1024 || streamMemory_>128*1024*1024 || streams_.size()>2048) {
            limit_=true; message_="历史记录已到保留上限；代理和周围对象继续实时更新，后续包不再保存。"; continue;
        }
        std::string key;
        if(p.protocol==6) {
            const auto base=p.directionKey(); const auto flow=p.flowKey();
            auto generation=generations_[flow];
            key=base+"/"+std::to_string(generation);
            auto it=streams_.find(key);
            if((p.flags&2) && !(p.flags&16) && it!=streams_.end() && it->second.initialized && (it->second.initialSequence!=p.sequence || it->second.closed)) {
                generation=++generations_[flow]; key=base+"/"+std::to_string(generation);
            }
            auto& stream=streams_[key]; const auto before=stream.bytes.size()+stream.pendingBytes;const auto beforeBytes=stream.bytes.size();
            Packet reverse;reverse.wireObservation=p.wireObservation;reverse.proxyConnection=p.proxyConnection;reverse.pid=p.pid;reverse.protocol=p.protocol;reverse.source=p.destination;reverse.destination=p.source;
            reverseStreams_[key]=reverse.directionKey()+"/"+std::to_string(generation);
            streamEndpoints_[key]={p.source,p.destination};
            if(p.proxyConnection)proxyStreamIds_[key]=p.proxyConnection;
            if(!diagnosticStreamIds_.contains(key)){
                auto id=diagnosticStreamIds_[key]=++diagnosticNextStreamId_;
                std::ostringstream details;details<<"stream="<<id<<" pid="<<p.pid<<" src_port="<<p.source.port<<" dst_port="<<p.destination.port
                    <<" attributed="<<p.attributed<<" outbound="<<p.outbound<<" ipv6="<<p.ipv6<<" first_flags="<<unsigned(p.flags)<<" world_inbound="<<(p.source.port==13328);
                diagnostics().write("tcp_stream_open",details.str());
            }
            stream.add(p);if(p.proxyConnection && !p.wireObservation && stream.limited){limit_=true;message_="历史消息流已到保留上限；代理和周围对象继续实时更新，后续包不再保存。";} streamMemory_+=stream.bytes.size()+stream.pendingBytes-before;
            if(stream.bytes.size()>beforeBytes)streamObservations_[key].push_back({stream.bytes.size(),packets_.size()+1,p.timeUs});
            if(!p.proxyConnection && (p.source.port==13328 || nearby_.contains(key))){
                nearby_[key].feed(stream,packets_.size()+1,p.timeUs);
                if(!p.payload.empty() && (followNearby_ || nearbyConnection_.empty()))nearbyConnection_=key;
            }
        }
        if(p.proxyConnection && !p.wireObservation){
            auto& c=recordedConnections_[p.proxyConnection];c.id=size_t(p.proxyConnection);c.pid=p.pid;
            c.client=p.outbound?p.source:p.destination;c.remote=p.outbound?p.destination:p.source;
            c.status="已保存的代理消息（只读回放）";
            if(!p.outbound && !p.payload.empty()){
                auto readResponse=[&](auto&& self,std::span<const uint8_t> bytes,unsigned depth)->void{
                    if(depth>4)return;
                    for(const auto& f:splitGameFrames(bytes).frames){auto part=bytes.subspan(f.offset,f.length);auto m=decodeGameFrame(part,true);
                        auto op=uint16_t(readInteger(part,f.prefixBytes,2,false));
                        if(op==0x3650)c.response=std::move(m);
                        else if(op==0x8a07 || op==0x8a09){c.guildResponse=std::move(m);c.guildOpcode=op;++c.guildReceived;c.guildStatus="已保存的军团响应（只读回放）";}
                        else if(!m.expanded.empty())self(self,m.expanded,depth+1);
                    }
                };readResponse(readResponse,p.payload,0);
            }
        }
        p.id=packets_.size()+1; memory_+=cost; packets_.push_back(std::move(p)); packetStreams_.push_back(key);
        refreshSelfPositions();
    }
    if(mode_.starts_with("LIVE") && !incoming.empty()){
        // The live model keeps updating after the packet archive fills. Bound
        // closed world snapshots independently; preserve active and selected worlds.
        std::vector<std::pair<uint64_t,std::string>> closed;
        for(const auto& [key,model]:nearby_)if(key.starts_with("proxy:") && model.closed && key!=nearbyConnection_)
            closed.emplace_back(proxyStreamIds_.contains(key)?proxyStreamIds_.at(key):0,key);
        std::sort(closed.begin(),closed.end());
        for(size_t i=0;i+16<closed.size();++i){const auto& key=closed[i].second;nearby_.erase(key);
            if(!streams_.contains(key)){proxyStreamIds_.erase(key);diagnosticStreamIds_.erase(key);diagnosticLastStreams_.erase(key);}
        }
    }
}
void App::toolbar() {
    ImGui::TextColored({0.27f,0.84f,0.75f,1},"AION2 / PIPE"); ImGui::SameLine(); ImGui::TextDisabled("二进制协议工作台     %s",mode_.c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(queryProxy_.running()); ImGui::SetNextItemWidth(170); ImGui::InputText("目标进程",process_,sizeof(process_)); ImGui::SameLine();
    ImGui::SetNextItemWidth(110);ImGui::InputInt("世界端口",&queryPort_);ImGui::SameLine();
    ImGui::TextDisabled("虚拟网卡模式");ImGui::SameLine();
    if(ImGui::Button("启用独立代理")) startCapture();
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!queryProxy_.running()); if(ImGui::Button("停止接管新连接")) {queryProxy_.drain();message_="已有连接继续转发和分析。关闭游戏连接后可关闭代理。";} ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(queryProxy_.running());
    if(ImGui::Button("加载示例")) demo(); ImGui::SameLine();
    if(ImGui::Button("打开会话 / PCAP")) {
        auto path=dialog(false,L"Aion2 session or PCAP\0*.a2session;*.pcap;*.cap\0All files\0*.*\0",L"pcap");
        if(!path.empty()) open(path);
    } ImGui::SameLine();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(packets_.empty());
    if(ImGui::Button(proxyData_?"保存代理会话":"导出 PCAP")) {
        auto path=proxyData_?dialog(true,L"Aion2 proxy session\0*.a2session\0",L"a2session"):dialog(true,L"PCAP files\0*.pcap\0",L"pcap");
        if(!path.empty()) try {if(proxyData_)saveSession(path,packets_,limit_ || (queryProxy_.running() && queryProxy_.stats().queueDropped));else savePcap(path,packets_); message_="已保存全部保留记录；代理会话包含明文，不含私钥或会话密钥。";} catch(const std::exception& e){message_=e.what();}
    } ImGui::EndDisabled(); ImGui::SameLine();
    if(proxyData_){if(ImGui::Button("导出原始 PCAP")){
        auto path=dialog(true,L"PCAP files\0*.pcap\0",L"pcap");if(!path.empty())try{std::vector<Packet> raw;for(const auto& p:packets_)if(p.wireObservation)raw.push_back(p);savePcap(path,raw);message_="已导出代理两段 TCP 连接的原始 IP 包，未混入解密消息。";}catch(const std::exception& e){message_=e.what();}
    }ImGui::SameLine();}
    ImGui::BeginDisabled(queryProxy_.running());if(ImGui::Button("清空")) {clear();mode_="EMPTY";message_="已清空分析会话。";}ImGui::EndDisabled();
    ImGui::SameLine();if(ImGui::Button("导出诊断日志")){
        diagnosticSnapshot(true);auto path=dialog(true,L"Diagnostic log\0*.log\0",L"log");
        if(!path.empty()){std::string error;if(diagnostics().exportTo(path,error))message_="诊断日志已导出，可将此 .log 文件发回排查。";else message_=error;}
    }
    if(ImGui::IsItemHovered())ImGui::SetTooltip("自动日志：%s\n出现问题后导出并发送 .log 文件。日志不含原始包内容。",utf8Path(diagnostics().path()).c_str());
    if(!diagnostics().error().empty())ImGui::TextColored({1,.6f,.3f,1},"%s",diagnostics().error().c_str());
    auto stats=queryProxy_.stats();
    ImGui::Text("保留 %zu 包    数据 %.2f MiB    目标进程 %zu    已匹配 %llu    队列丢弃 %llu    未解析 %llu",packets_.size(),double(memory_+streamMemory_)/1048576,stats.processes,stats.matched,stats.queueDropped,stats.unsupported);
    ImGui::SameLine();ImGui::TextColored({0.27f,0.84f,0.75f,1},"%s",queryProxy_.routingMode().c_str());
    if(queryProxy_.running()) {ImGui::SameLine(); ImGui::TextColored({0.3f,0.9f,0.6f,1},"  PROXY");}
    ImGui::TextWrapped("%s",message_.c_str());
    if(mode_.starts_with("LIVE")) ImGui::TextDisabled("%s",queryProxy_.status().c_str());
}
void App::packetsView() {
    ImGui::TextDisabled("01  /  数据包");
    if(proxyData_){ImGui::SameLine();ImGui::SetNextItemWidth(220);if(ImGui::Combo("记录",&recordKind_,"解密消息\0原始网络包（两段连接）\0"))selected_=-1;}

    ImGui::SetNextItemWidth(200); ImGui::InputTextWithHint("##filter","地址 / 端口 / PID",filter_,sizeof(filter_)); ImGui::SameLine();
    ImGui::SetNextItemWidth(115); ImGui::Combo("##protocol",&transport_,"全部协议\0TCP\0UDP\0"); ImGui::SameLine();
    ImGui::SetNextItemWidth(115); ImGui::Combo("##direction",&direction_,"全部方向\0发送\0接收\0"); ImGui::SameLine(); ImGui::Checkbox("仅含负载",&payloadOnly_); ImGui::SameLine(); ImGui::Checkbox("跟随",&autoScroll_);
    std::vector<int> visible; visible.reserve(packets_.size());
    for(int i=0;i<int(packets_.size());++i) {
        const auto& p=packets_[i];
        if(proxyData_ && p.wireObservation!=(recordKind_==1))continue;
        if(payloadOnly_ && p.payload.empty()) continue;
        if(transport_ && p.protocol!=(transport_==1?6:17)) continue;
        if(direction_ && (!p.attributed || p.outbound!=(direction_==1))) continue;
        if(filter_[0] && (p.source.text()+" "+p.destination.text()+" "+std::to_string(p.pid)).find(filter_)==std::string::npos) continue;
        visible.push_back(i);
    }
    const auto flags=ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersInnerV;
    if(ImGui::BeginTable("packets",8,flags,ImVec2(0,0))) {
        ImGui::TableSetupScrollFreeze(0,1);
        ImGui::TableSetupColumn("#",ImGuiTableColumnFlags_WidthFixed,50);
        ImGui::TableSetupColumn("时间 (s)",ImGuiTableColumnFlags_WidthFixed,86);
        ImGui::TableSetupColumn("PID",ImGuiTableColumnFlags_WidthFixed,62);
        ImGui::TableSetupColumn("方向",ImGuiTableColumnFlags_WidthFixed,48);
        ImGui::TableSetupColumn("协议",ImGuiTableColumnFlags_WidthFixed,45);
        ImGui::TableSetupColumn("源端点"); ImGui::TableSetupColumn("目的端点");
        ImGui::TableSetupColumn("负载",ImGuiTableColumnFlags_WidthFixed,54); ImGui::TableHeadersRow();
        ImGuiListClipper clip; clip.Begin(int(visible.size()));
        while(clip.Step()) for(int row=clip.DisplayStart;row<clip.DisplayEnd;++row) {
            int index=visible[row]; const auto& p=packets_[index]; ImGui::PushID(index); ImGui::TableNextRow(); ImGui::TableNextColumn();
            if(ImGui::Selectable(std::to_string(p.id).c_str(),selected_==index,ImGuiSelectableFlags_SpanAllColumns)) {selected_=index; byteOffset_=0;}
            ImGui::TableNextColumn(); ImGui::Text("%.4f",double(int64_t(p.timeUs)-int64_t(packets_.front().timeUs))/1000000);
            ImGui::TableNextColumn(); if(p.attributed) ImGui::Text("%u",p.pid); else ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.attributed?(p.outbound?"TX":"RX"):"--");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.protocol==6?"TCP":"UDP");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.source.text().c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.destination.text().c_str());
            ImGui::TableNextColumn(); ImGui::Text("%zu",p.payload.size()); ImGui::PopID();
        }
        if(autoScroll_ && queryProxy_.running()) ImGui::SetScrollY(ImGui::GetScrollMaxY());
        ImGui::EndTable();
    }
}
void App::hexView(std::span<const uint8_t> bytes) {
    ImGui::SetNextItemWidth(250); ImGui::InputTextWithHint("##search","HEX: 41 49 4F 4E",search_,sizeof(search_)); ImGui::SameLine();
    if(ImGui::Button("查找下一个")) {
        auto pattern=parseHex(search_);
        if(!pattern || pattern->empty()) message_="请输入成对十六进制字节，如 41 49 4F 4E。";
        else {
            auto begin=bytes.begin()+std::min(bytes.size(),size_t(std::max(0,byteOffset_+1)));
            auto it=std::search(begin,bytes.end(),pattern->begin(),pattern->end());
            if(it==bytes.end()) it=std::search(bytes.begin(),bytes.end(),pattern->begin(),pattern->end());
            if(it!=bytes.end()) {byteOffset_=int(it-bytes.begin()); scrollToByte_=true; message_="匹配位置："+std::to_string(byteOffset_);} else message_="未找到匹配字节。";
        }
    } ImGui::SameLine(); if(ImGui::Button("复制 HEX")) ImGui::SetClipboardText(hex(bytes.first(std::min<size_t>(bytes.size(),65536))).c_str());
    ImGui::SameLine(); ImGui::TextDisabled("复制最多 64 KiB");
    ImGui::BeginChild("hexscroll",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    if(scrollToByte_) {ImGui::SetScrollY(float(byteOffset_/16)*ImGui::GetTextLineHeightWithSpacing()); scrollToByte_=false;}
    const float cell=ImGui::CalcTextSize("FF").x+10;
    const float offsetWidth=ImGui::CalcTextSize("00000000").x+20;
    ImGuiListClipper clip; clip.Begin(int((bytes.size()+15)/16),ImGui::GetTextLineHeightWithSpacing());
    while(clip.Step()) for(int row=clip.DisplayStart;row<clip.DisplayEnd;++row) {
        size_t at=size_t(row)*16; ImGui::TextDisabled("%08X",unsigned(at));
        std::string ascii;
        for(size_t c=0;c<16 && at+c<bytes.size();++c) {
            ImGui::SameLine(offsetWidth+float(c)*cell); ImGui::PushID(int(at+c)); char label[4]; snprintf(label,sizeof(label),"%02X",bytes[at+c]);
            if(ImGui::Selectable(label,byteOffset_==int(at+c),0,ImVec2(cell-3,ImGui::GetTextLineHeight()))) byteOffset_=int(at+c);
            ImGui::PopID(); auto b=bytes[at+c]; ascii+=b>=32 && b<127?char(b):'.';
        }
        ImGui::SameLine(offsetWidth+16*cell+10); ImGui::TextUnformatted(ascii.c_str());
    }
    ImGui::EndChild();
}
void App::fieldsView(std::span<const uint8_t> bytes) {
    ImGui::SetNextItemWidth(150); ImGui::InputInt("字节偏移",&byteOffset_); byteOffset_=std::max(0,byteOffset_);
    ImGui::Checkbox("大端序 (Big Endian)",&bigEndian_);
    size_t o=size_t(byteOffset_);
    if(o>=bytes.size()) {ImGui::TextDisabled("偏移超出数据范围。"); return;}
    ImGui::Text("偏移 0x%X / %d",byteOffset_,byteOffset_);
    for(size_t width:{1u,2u,4u,8u}) if(width<=bytes.size()-o) {
        auto value=readInteger(bytes,o,width,bigEndian_);
        int64_t signedValue{};
        if(width==8) std::memcpy(&signedValue,&value,8);
        else signedValue=(value&(uint64_t(1)<<(width*8-1))) ? int64_t(value)-int64_t(uint64_t(1)<<(width*8)) : int64_t(value);
        ImGui::Text("u%zu: %llu    i%zu: %lld    hex: 0x%llX",width*8,value,width*8,signedValue,value);
    }
    if(bytes.size()-o>=4) {uint32_t bits=uint32_t(readInteger(bytes,o,4,bigEndian_)); float v; std::memcpy(&v,&bits,4); ImGui::Text("float32: %.9g",double(v));}
    if(bytes.size()-o>=8) {uint64_t bits=readInteger(bytes,o,8,bigEndian_); double v; std::memcpy(&v,&bits,8); ImGui::Text("float64: %.17g",v);}
    uint64_t varint=0; bool valid=false;
    for(size_t i=0;i<10 && o+i<bytes.size();++i) {uint8_t b=bytes[o+i]; if(i==9 && b>1) break; varint|=uint64_t(b&127)<<(7*i); if(!(b&128)){valid=true;break;}}
    if(valid) ImGui::Text("unsigned LEB128: %llu",varint);
    ImGui::Separator(); ImGui::TextDisabled("可打印 ASCII 片段（从选中偏移起，最多 512 字节）");
    std::string ascii; for(auto c:bytes.subspan(o,std::min<size_t>(512,bytes.size()-o))) ascii+=c>=32&&c<127?char(c):'.';
    ImGui::TextWrapped("%s",ascii.c_str());
}
void App::framesView(std::span<const uint8_t> bytes) {
    ImGui::TextWrapped("探索性拆包：请依据调试器与样本确认长度字段，不将结果自动认定为游戏协议。");
    if(ImGui::Button("样本候选：u8 长度 - 3")) {lengthOffset_=0; lengthWidth_=1; headerSize_=1; opcodeWidth_=0; includesHeader_=true; bigEndian_=false; lengthAdjustment_=-3; startOffset_=0;}
    ImGui::SameLine(); ImGui::TextDisabled("仅小包样本假设；禁用消息号解释");
    ImGui::PushItemWidth(90);
    ImGui::InputInt("起始偏移",&startOffset_); ImGui::SameLine(); ImGui::InputInt("长度偏移",&lengthOffset_); ImGui::SameLine(); ImGui::InputInt("长度宽度",&lengthWidth_);
    ImGui::InputInt("头部字节",&headerSize_); ImGui::SameLine(); ImGui::InputInt("消息号偏移",&opcodeOffset_); ImGui::SameLine(); ImGui::InputInt("消息号宽度",&opcodeWidth_);
    ImGui::InputInt("最大帧",&maxFrame_,0,0); ImGui::SameLine(); ImGui::Checkbox("长度含头部",&includesHeader_); ImGui::SameLine(); ImGui::Checkbox("大端序",&bigEndian_); ImGui::PopItemWidth();
    ImGui::SetNextItemWidth(90); ImGui::InputInt("长度修正（有符号）",&lengthAdjustment_);
    if(startOffset_<0 || size_t(startOffset_)>bytes.size() || lengthOffset_<0 || lengthWidth_<0 || headerSize_<0 || opcodeOffset_<0 || opcodeWidth_<0 || maxFrame_<0) {ImGui::TextDisabled("参数超出范围。"); return;}
    Framing rule{size_t(lengthOffset_),size_t(lengthWidth_),bigEndian_,includesHeader_,size_t(headerSize_),size_t(maxFrame_),size_t(opcodeOffset_),size_t(opcodeWidth_)};
    rule.adjustment=lengthAdjustment_;
    auto result=splitFrames(bytes.subspan(size_t(startOffset_)),rule);
    ImGui::Text("%zu 个完整帧，消耗 %zu 字节 | %s",result.frames.size(),result.consumed,result.status.c_str());
    if(ImGui::BeginTable("frames",4,ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg,ImVec2(0,0))) {
        ImGui::TableSetupColumn("偏移"); ImGui::TableSetupColumn("长度"); ImGui::TableSetupColumn("消息号（候选）"); ImGui::TableSetupColumn("前 16 字节"); ImGui::TableHeadersRow();
        ImGuiListClipper clip; clip.Begin(int(result.frames.size()));
        while(clip.Step()) for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i) {
            const auto& f=result.frames[i]; size_t offset=size_t(startOffset_)+f.offset;
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if(ImGui::Selectable(std::to_string(offset).c_str(),byteOffset_==int(offset),ImGuiSelectableFlags_SpanAllColumns)) byteOffset_=int(offset);
            ImGui::TableNextColumn(); ImGui::Text("%zu",f.length); ImGui::TableNextColumn(); if(f.hasOpcode) ImGui::Text("0x%llX (%llu)",f.opcode,f.opcode); else ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(hex(bytes.subspan(offset,std::min<size_t>(16,f.length))).c_str());
        } ImGui::EndTable();
    }
}
void App::gameView(std::span<const uint8_t> bytes,const Packet& p) {
    if(p.wireObservation){ImGui::TextWrapped("这是代理两段连接的原始 IP 记录；请切换到解密消息查看自动协议解析。HEX、字段与自定义拆包仍可使用。");return;}
    if(expandedPacket_!=p.id){expandedGame_.clear();decryptedGame_.clear();expandedPacket_=p.id;}
    ImGui::SetNextItemWidth(280);
    if(ImGui::Combo("解释方式",&gameMode_,"连接自动判断\0游戏入站明文（手动）\0出站 / 原始正文\0登录入站明文（手动）\0")){expandedGame_.clear();decryptedGame_.clear();}
    if(source_==1) {ImGui::TextWrapped(p.proxyConnection?"请选择消息明文或同方向消息流进行协议分析。":"请选择负载或同方向 TCP 流；原始 IP 头不属于游戏消息。");return;}
    const bool detectedWorld=selected_>=0 && size_t(selected_)<packetStreams_.size() && nearby_.contains(packetStreams_[selected_]);
    if(gameMode_==0 && !p.proxyConnection && !detectedWorld && p.source.port!=13328 && p.destination.port!=13328 && p.source.port!=13700 && p.destination.port!=13700) {ImGui::TextWrapped("此连接尚未识别服务；可手动选择对应服务进行检查。");return;}
    const auto profile=(gameMode_==3 || (gameMode_!=1 && !detectedWorld && (p.source.port==13700 || p.destination.port==13700)))?GameProfile::Login:GameProfile::World;
    bool inbound=gameMode_==1 || gameMode_==3 || (gameMode_==0 && (detectedWorld || p.source.port==13328 || p.source.port==13700));
    ImGui::TextDisabled("当前服务：%s",profile==GameProfile::Login?"登录服务":"游戏世界");
    if(!inbound && !p.proxyConnection) {
        bool decode=pendingDecrypt_;pendingDecrypt_=false;
        if(ImGui::Button("导入会话状态并解密")) {
            auto path=dialog(false,L"Aion2 session state\0*.a2cs\0",L"a2cs");
            if(!path.empty())try{cipherSnapshot_=loadCipherSnapshot(path);decode=true;}catch(const std::exception& e){message_=e.what();}
        }
        if(cipherSnapshot_){ImGui::SameLine();if(ImGui::Button("重新解密当前流"))decode=true;}
        if(decode && cipherSnapshot_)try{
            uint32_t seq=p.sequence+((p.flags&2)?1:0);
            if(source_==2){const auto& stream=streams_.at(packetStreams_[selected_]);seq=stream.initialSequence+(stream.startedWithSyn?1:0);}
            decryptedGame_=decryptGameStream(bytes,seq,p.source,p.destination,*cipherSnapshot_);expandedGame_.clear();gameFrame_=0;
            message_="已从状态锚点还原连续出站帧。解密结果是本次快照，需重新解密才能更新。";
        }catch(const std::exception& e){decryptedGame_.clear();message_=e.what();}
    }
    const bool decrypted=!decryptedGame_.empty();
    const bool proxyPlain=p.proxyConnection && p.plaintext;
    if(p.proxyConnection)inbound=!p.outbound;
    if(decrypted){bytes=decryptedGame_;ImGui::TextDisabled("已按会话状态还原正文；下列偏移属于解密快照，不对应原始 HEX。");}
    if(!expandedGame_.empty()) {
        if(ImGui::Button("返回网络数据")){expandedGame_.clear();gameFrame_=0;}
        else {bytes=expandedGame_;inbound=true;ImGui::SameLine();ImGui::TextDisabled("下列偏移属于解压数据，不能直接定位原始 HEX");}
    }
    ImGui::TextWrapped("2026-09-24 客户端分析配置 | %s | 结构完整不代表全部业务含义已确认。",proxyPlain?"独立代理明文":decrypted?"历史会话锚点出站明文":inbound?"入站明文":"仅长度，正文待分析");
    auto frames=splitGameFrames(bytes);
    ImGui::Text("%zu 帧 / 消耗 %zu 字节 / %s",frames.frames.size(),frames.consumed,frames.status.c_str());
    if(frames.frames.empty())return;
    gameFrame_=std::clamp(gameFrame_,0,int(frames.frames.size())-1);
    ImGui::SetNextItemWidth(200);ImGui::SliderInt("消息序号",&gameFrame_,0,int(frames.frames.size())-1);
    ImGui::SameLine();if(ImGui::Button("上一条") && gameFrame_>0)--gameFrame_;
    ImGui::SameLine();if(ImGui::Button("下一条") && gameFrame_+1<int(frames.frames.size()))++gameFrame_;
    const auto& frame=frames.frames[gameFrame_];auto decoded=decodeGameFrame(bytes.subspan(frame.offset,frame.length),inbound||decrypted||proxyPlain,decrypted||(proxyPlain && p.outbound),profile);
    ImGui::Text("%s | 偏移 %zu / %zu 字节 | %s",decoded.name.c_str(),frame.offset,frame.length,decoded.status.c_str());
    if(!decoded.expanded.empty() && ImGui::Button("查看解压后的消息")){expandedGame_=std::move(decoded.expanded);gameFrame_=0;return;}
    if(!decoded.config.status.empty()) {
        ImGui::TextWrapped("角色配置：%s",decoded.config.status.c_str());
        if(decoded.config.complete && ImGui::CollapsingHeader("角色配置字段（解压后的 JSON）",ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("%zu 个顶层键 / %zu 字节；键名来自原文，具体单位和枚举仍待确认",decoded.config.topLevelKeys,decoded.config.expandedBytes);
            if(ImGui::BeginTable("configfields",3,ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable,ImVec2(0,240))) {
                ImGui::TableSetupColumn("JSON 路径");ImGui::TableSetupColumn("类型",ImGuiTableColumnFlags_WidthFixed,75);ImGui::TableSetupColumn("值");ImGui::TableHeadersRow();
                for(const auto& value:decoded.config.values){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextWrapped("%s",value.path.c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(value.type.c_str());ImGui::TableNextColumn();ImGui::TextWrapped("%s",value.value.c_str());}
                ImGui::EndTable();
            }
        }
    }
    if(ImGui::BeginTable("gamefields",5,ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable,ImVec2(0,0))) {
        ImGui::TableSetupColumn("偏移 / 长度",ImGuiTableColumnFlags_WidthFixed,110);
        ImGui::TableSetupColumn("字段");ImGui::TableSetupColumn("类型",ImGuiTableColumnFlags_WidthFixed,95);
        ImGui::TableSetupColumn("值");ImGui::TableSetupColumn("依据 / 确认程度");ImGui::TableHeadersRow();
        for(size_t i=0;i<decoded.fields.size();++i) {
            const auto& f=decoded.fields[i];ImGui::PushID(int(i));ImGui::TableNextRow();ImGui::TableNextColumn();
            auto at=frame.offset+f.offset;std::string where=std::to_string(at)+" / "+std::to_string(f.size);
            if(f.bit>=0)where+=" b"+std::to_string(f.bit);
            if(ImGui::Selectable(where.c_str(),!decrypted && expandedGame_.empty() && byteOffset_==int(at),ImGuiSelectableFlags_SpanAllColumns) && !decrypted && expandedGame_.empty()){byteOffset_=int(at);scrollToByte_=true;}
            ImGui::TableNextColumn();ImGui::TextWrapped("%s",f.name.c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(f.type.c_str());
            ImGui::TableNextColumn();ImGui::TextWrapped("%s",f.value.c_str());ImGui::TableNextColumn();
            if(!f.meaningKnown)ImGui::TextColored({1,.75f,.3f,1},"含义待确认");
            ImGui::TextWrapped("%s",f.evidence.c_str());ImGui::PopID();
        }ImGui::EndTable();
    }
}
void App::inspector() {
    ImGui::TextDisabled("02  /  字节与协议");
    if(selected_<0 || size_t(selected_)>=packets_.size()) {ImGui::TextWrapped("选择一个数据包查看十六进制、数值字段和 TCP 流。首次使用可以点击“加载示例”。"); return;}
    const auto& p=packets_[selected_];
    ImGui::SetNextItemWidth(210); if(ImGui::Combo("数据来源",&source_,(p.proxyConnection && !p.wireObservation)?"当前消息明文\0传输正文（未解码）\0同方向消息流\0":"当前包负载\0原始 IP 包\0同方向 TCP 流\0")){expandedGame_.clear();decryptedGame_.clear();gameFrame_=0;}
    ImGui::SameLine(); const bool exportBinary=ImGui::Button("导出 BIN");
    std::span<const uint8_t> data=p.payload;
    if(source_==1) data=p.raw;
    if(p.proxyConnection && !p.wireObservation)ImGui::TextDisabled("代理连接 %llu · %s · 序号为分析字节偏移，非线上 TCP 序号",p.proxyConnection,p.toolGenerated?"工具发起":"游戏通信");
    if(source_==2) {
        if(p.protocol!=6) {ImGui::TextDisabled("UDP 数据报独立，不进行 TCP 重组。"); return;}
        auto& s=streams_.at(packetStreams_[selected_]); data=s.bytes;
        ImGui::Text("连续数据 %zu 字节 / 重传 %zu / 等待乱序 %zu",s.bytes.size(),s.duplicateBytes,s.pendingBytes);
        if(!s.startedWithSyn) ImGui::TextColored({1,.75f,.3f,1},"中途捕获：起点来自首个观察到的段，之前的数据未知。");
        if(s.hasGap()) ImGui::TextColored({1,.75f,.3f,1},"流存在缺口；仅显示缺口之前连续的字节，不拼接跨缺口数据。");
        if(s.limited) ImGui::TextColored({1,.5f,.4f,1},"已达到流内存限制，流已停止增长。");
    }
    if(exportBinary) {
        auto path=dialog(true,L"Binary files\0*.bin\0",L"bin"); if(!path.empty()) try {saveBinary(path,data); message_="已导出当前选中数据源。";} catch(const std::exception& e){message_=e.what();}
    }
    // Entropy is sampled to keep multi-megabyte stream inspection responsive.
    ImGui::Text("%zu 字节    熵 %.3f bit/byte（前 64 KiB）    %s %u    flags 0x%02X",data.size(),entropy(data.first(std::min<size_t>(65536,data.size()))),(p.proxyConnection && !p.wireObservation)?"分析偏移":"TCP seq",p.sequence,p.flags);
    if(ImGui::BeginTabBar("inspect")) {
        if(ImGui::BeginTabItem("HEX / ASCII")) {hexView(data); ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("字段解释")) {fieldsView(data); ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Aion2 协议",nullptr,selectGameTab_?ImGuiTabItemFlags_SetSelected:0)) {gameView(data,p);ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("长度拆包",nullptr,selectFrameTab_?ImGuiTabItemFlags_SetSelected:0)) {framesView(data); ImGui::EndTabItem();}
        selectFrameTab_=false;
        selectGameTab_=false;
        ImGui::EndTabBar();
    }
}
void App::nearbyView() {
    ImGui::TextColored({.27f,.84f,.75f,1},"周围对象数组");
    ImGui::SameLine();ImGui::TextDisabled("按当前连接已收到的消息更新 · 不同连接独立");
    ImGui::SetNextItemWidth(620);
    if(ImGui::BeginCombo("连接",nearbyConnection_.empty()?"等待世界连接入站数据":nearbyConnection_.c_str())){
        for(const auto& [key,model]:nearby_)if(ImGui::Selectable(key.c_str(),key==nearbyConnection_)){nearbyConnection_=key;nearbySelected_.reset();followNearby_=false;}
        ImGui::EndCombo();
    }
    ImGui::SameLine();ImGui::Checkbox("跟随最新连接",&followNearby_);
    auto connection=nearby_.find(nearbyConnection_);
    if(connection==nearby_.end()){ImGui::Spacing();ImGui::TextWrapped("尚未识别到世界对象消息。支持标准端口和按消息结构识别转发连接。请先启用代理，再重新登录并进入场景；若仍为空，请导出诊断日志。");return;}
    const auto& model=connection->second;
    size_t present=0;for(const auto& [key,o]:model.objects())present+=o.present;
    ImGui::Text("当前观察到 %zu 个对象    含离开记录 %zu    更新 %zu 次    场景 %zu",present,model.objects().size(),model.updates,model.scene);
    if(model.midstream || model.gap || model.limited || model.closed || limit_ || (queryProxy_.running() && queryProxy_.stats().queueDropped)){
        ImGui::TextColored({1,.75f,.3f,1},"%s%s%s%s%s",model.midstream?"中途捕获，可能缺少出现消息。  ":"",model.gap?"TCP 存在缺口，列表可能过时。  ":"",model.limited?"对象解析达到容量限制，数据不完整。  ":limit_?(proxyData_ && mode_.starts_with("LIVE")?"历史包已满，对象继续实时更新。  ":"历史记录不完整。  "):"",model.closed?"连接已关闭，显示历史快照。  ":"",(queryProxy_.running() && queryProxy_.stats().queueDropped)?"捕获队列曾丢包。":"");
    }
    ImGui::TextDisabled("%zu 条消息尚未完整解析；这里只表示已观察到的对象。网络位置包含已确认的移动增量，单位待确认；* 表示基准失效，等待绝对位置。",model.unparsed);
    if(proxyData_)ImGui::TextDisabled("独立代理自动还原自身上报 · 更新 %zu 次 · 无需导入会话状态",proxySelfUpdates_);
    else {
    if(ImGui::Button("导入历史自身移动会话")){
        auto path=dialog(false,L"Aion2 cipher state\0*.a2cs\0",L"a2cs");if(!path.empty())openCipherState(path);
    }
    ImGui::SameLine();
    if(!cipherSnapshot_)ImGui::TextDisabled("未导入同次连接的会话状态，自身加密移动尚不可读");
    else if(auto reverse=reverseStreams_.find(nearbyConnection_);reverse!=reverseStreams_.end()){
        auto tx=selfMovement_.find(reverse->second);
        if(tx!=selfMovement_.end()){
            ImGui::TextDisabled("自身上报更新 %zu 次 · 未解析发送消息 %zu 条",tx->second.updates,tx->second.incomplete);
            if(ImGui::IsItemHovered())ImGui::SetTooltip("发送流：%s",tx->second.status.c_str());
        }
        else ImGui::TextDisabled("会话状态尚未匹配当前连接");
    }
    }
    for(const auto& [key,o]:model.objects())if(o.isSelf && o.present && o.positionSource=="appearance"){
        ImGui::TextColored({1,.75f,.3f,1},"自身尚未解析到坐标更新：当前显示入场位置。血量、状态更新不会刷新位置。");break;
    }
    for(const auto& [key,o]:model.objects())if(o.isSelf && o.present && o.positionSource=="client_report"){
        ImGui::TextDisabled("自身显示最近已解析的客户端上报位置，尚不代表服务器确认的位置。");break;
    }
    if(!model.framingStatus.empty() && model.framingStatus!="完整")ImGui::TextDisabled("流边界：%s",model.framingStatus.c_str());
    ImGui::SetNextItemWidth(270);ImGui::InputTextWithHint("##nearbySearch","名称 / 实体编号 / 公会",nearbyFilter_,sizeof(nearbyFilter_));ImGui::SameLine();
    ImGui::SetNextItemWidth(135);ImGui::Combo("##nearbyKind",&nearbyKind_,"全部对象\0玩家\0NPC/怪物\0环境对象\0拟态对象\0类型待确认\0");ImGui::SameLine();
    ImGui::Checkbox("显示已离开",&showDeparted_);ImGui::SameLine();
    if(ImGui::Button("导出 JSON 数组")){
        auto path=dialog(true,L"JSON files\0*.json\0",L"json");
        if(!path.empty())try {auto json=model.json(showDeparted_);saveBinary(path,{reinterpret_cast<const uint8_t*>(json.data()),json.size()});message_="已导出当前连接的全部类型对象数组；是否包含离开记录遵循勾选项。";}catch(const std::exception& e){message_=e.what();}
    }
    auto& skinPref=skinPreferences();
    if(ImGui::Checkbox("重点时装优先",&skinPref.data.prioritize))skinPref.save();ImGui::SameLine();
    if(ImGui::Button("管理重点时装"))ImGui::OpenPopup("manageSkinFocus");
    if(ImGui::BeginPopup("manageSkinFocus")){
        ImGui::TextUnformatted("标记的是时装款式；穿同款的玩家都会被突出显示。");
        if(skinPref.data.favorites.empty())ImGui::TextDisabled("暂无重点款式。点击列表“标记”或在详情中勾选。");
        uint32_t remove=0;ImGui::BeginChild("favoritesList",ImVec2(520,220));
        for(auto id:skinPref.data.favorites){ImGui::PushID(int(id));if(ImGui::SmallButton("移除"))remove=id;ImGui::SameLine();ImGui::TextWrapped("%s [%u]",skinCatalog().name(id,0).c_str(),id);ImGui::PopID();}
        ImGui::EndChild();if(remove)skinPref.toggle(remove);ImGui::EndPopup();
    }
    if(!skinPref.error.empty()){ImGui::TextWrapped("重点设置保存异常：%s",skinPref.error.c_str());ImGui::SameLine();if(ImGui::SmallButton("重试保存"))skinPref.save();}
    auto value=[](const NearbyObject& o,const char* key)->std::string{auto it=o.values.find(key);return it==o.values.end()?"—":it->second;};
    const auto tableFlags=ImGuiTableFlags_ScrollY|ImGuiTableFlags_ScrollX|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Hideable;
    float h=std::max(120.0f,ImGui::GetContentRegionAvail().y-155);
    if(ImGui::BeginTable("nearbyObjectsSkinOverview",15,tableFlags,ImVec2(0,h))){
        const char* labels[]={"实体编号","类型","名称","时装概览","配色","重点时装","等级","公会","模板 ID","角色数据库 ID","状态","更新包","HP / 上限","MP / 上限","最近网络位置 X / Y / Z"};
        const float widths[]={85,105,160,260,155,90,55,160,95,160,90,75,155,130,265};
        for(size_t i=0;i<std::size(labels);++i)ImGui::TableSetupColumn(labels[i],ImGuiTableColumnFlags_WidthFixed,widths[i]);
        ImGui::TableSetupScrollFreeze(3,1);ImGui::TableHeadersRow();
        std::vector<const NearbyObject*> rows;for(const auto& [key,o]:model.objects())rows.push_back(&o);
        if(skinPref.data.prioritize)std::stable_sort(rows.begin(),rows.end(),[&](const auto* a,const auto* b){
            auto x=skinPref.data.matches(a->skins),y=skinPref.data.matches(b->skins);if((x>0)!=(y>0))return x>0;if(x && a->present!=b->present)return a->present;if(x!=y)return x>y;return a->key<b->key;});
        for(const auto* row:rows){const auto& o=*row;auto key=o.key;
            if(!showDeparted_ && !o.present)continue;
            if(nearbyKind_ && (nearbyKind_==5?o.kind!=ObjectKind::Unknown:int(o.kind)!=nearbyKind_))continue;
            auto id=std::to_string(key),name=value(o,"name"),guild=value(o,"guild");
            if(*nearbyFilter_ && (id+" "+name+" "+guild).find(nearbyFilter_)==std::string::npos)continue;
            const auto focusCount=skinPref.data.matches(o.skins);
            ImGui::PushID(id.c_str());ImGui::TableNextRow();
            if(focusCount)ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,IM_COL32(83,65,25,150));
            ImGui::TableNextColumn();
            if(ImGui::Selectable(id.c_str(),nearbySelected_==key,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowOverlap))nearbySelected_=key;
            ImGui::TableNextColumn();ImGui::TextUnformatted(o.isSelf?"玩家（自身）":objectKindName(o.kind));ImGui::TableNextColumn();ImGui::TextUnformatted(name.c_str());
            const auto race=skinRace(value(o,"race"));
            std::vector<const SkinEquipment*> visible;for(const auto& skin:o.skins)if(skin.display && skin.skinId)visible.push_back(&skin);
            std::stable_sort(visible.begin(),visible.end(),[](const auto* a,const auto* b){auto rank=[](uint8_t slot){return slot==4?0:slot==2?1:slot==1?2:int(slot)+3;};return rank(a->visualSlot)<rank(b->visualSlot);});
            ImGui::TableNextColumn();
            if(visible.empty())ImGui::TextDisabled(o.skins.empty()?"外观资料未获取":"无已显示时装");
            else {auto summary=skinLabel(*visible.front(),race)+" · "+std::to_string(visible.size())+" 件";ImGui::TextUnformatted(summary.c_str());
                if(ImGui::IsItemHovered()){ImGui::BeginTooltip();for(const auto* skin:visible)ImGui::Text("%s：%s%s",visualSlotName(skin->visualSlot),skinLabel(*skin,race).c_str(),skinPref.data.favorites.contains(skin->skinId)?" [重点]":"");ImGui::EndTooltip();}}
            ImGui::TableNextColumn();
            size_t colorIndex=0,totalColors=0;
            for(const auto* skin:visible)for(const auto& dye:skin->dyes){
                auto swatch=[&](const std::array<uint8_t,3>& rgb,const char* kind){++totalColors;if(colorIndex>=5)return;if(colorIndex)ImGui::SameLine(0,3);ImGui::PushID(int(colorIndex++));
                    ImGui::ColorButton("##overviewColor",ImVec4(rgb[0]/255.f,rgb[1]/255.f,rgb[2]/255.f,1),ImGuiColorEditFlags_NoTooltip,ImVec2(18,18));
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s\n%s · 槽 %u · %s\n%s / RGB %u, %u, %u",skinLabel(*skin,race).c_str(),visualSlotName(skin->visualSlot),dye.slot,kind,skinColorHex(rgb).c_str(),rgb[0],rgb[1],rgb[2]);ImGui::PopID();};
                if(!dye.remove)swatch(dye.rgb,"染色");if(dye.patternId && !dye.patternRemove)swatch(dye.patternRgb,"图案");
            }
            if(!totalColors)ImGui::TextDisabled(visible.empty()?"—":"默认色");else if(totalColors>5){ImGui::SameLine(0,3);ImGui::Text("+%zu",totalColors-5);}
            ImGui::TableNextColumn();
            if(focusCount)ImGui::TextColored(ImVec4(1,.8f,.3f,1),"重点 %zu",focusCount);
            ImGui::BeginDisabled(visible.empty());if(ImGui::SmallButton("标记"))ImGui::OpenPopup("focusSkins");ImGui::EndDisabled();
            if(ImGui::BeginPopup("focusSkins")){std::set<uint32_t> shown;for(const auto* skin:visible)if(shown.insert(skin->skinId).second){ImGui::PushID(int(skin->skinId));bool active=skinPref.data.favorites.contains(skin->skinId);if(ImGui::Checkbox(skinLabel(*skin,race).c_str(),&active))skinPref.toggle(skin->skinId);ImGui::PopID();}ImGui::EndPopup();}
            ImGui::TableNextColumn();ImGui::TextUnformatted(value(o,"level").c_str());
            ImGui::TableNextColumn();ImGui::TextUnformatted(guild.c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(value(o,"template_id").c_str());
            ImGui::TableNextColumn();ImGui::TextUnformatted(value(o,"character_dbid").c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(!o.present?"已离开":o.isSelf?"自身":o.appearanceSeen?"已出现":"缺少出现包");
            ImGui::TableNextColumn();ImGui::Text("%llu",o.lastPacket);
            ImGui::TableNextColumn();ImGui::Text("%s / %s",value(o,"hp").c_str(),value(o,"max_hp").c_str());
            ImGui::TableNextColumn();ImGui::Text("%s / %s",value(o,"mp").c_str(),value(o,"max_mp").c_str());
            ImGui::TableNextColumn();if(o.position)ImGui::Text("%.2f / %.2f / %.2f%s",(*o.position)[0],(*o.position)[1],(*o.position)[2],o.positionNeedsRefresh?" *":"");else ImGui::TextUnformatted("—");
            if(ImGui::IsItemHovered())ImGui::SetTooltip("位置来源：%s；位置更新包：%llu%s",o.positionSource=="appearance"?"对象出现时的位置":o.positionSource=="client_report"?"客户端自身位置上报（非服务器确认）":o.positionSource=="server_correction"?"服务器位置校正（移动被拒绝）":o.positionSource=="server_attack"?"服务器攻击位移位置":o.positionSource=="delta"?"绝对基准 + 移动增量":o.position?"绝对位置更新":"尚无基准",o.positionPacket,o.positionNeedsRefresh?"；等待绝对位置刷新":"");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if(nearbySelected_){auto it=model.objects().find(*nearbySelected_);if(it!=model.objects().end()){
        const auto& o=it->second;
        ImGui::Text("实体 %u · %s · %s",o.key,objectKindName(o.kind),o.lastMessage.c_str());ImGui::SameLine();
        ImGui::BeginDisabled(!o.lastPacket || o.lastPacket>packets_.size());
        if(ImGui::SmallButton("查看更新包") && o.lastPacket && o.lastPacket<=packets_.size()){selected_=int(o.lastPacket-1);source_=2;selectGameTab_=true;expandedGame_.clear();decryptedGame_.clear();}
        ImGui::EndDisabled();
        if(o.lastPacket>packets_.size()){ImGui::SameLine();ImGui::TextDisabled("此更新包未保留，仅更新实时对象状态");}
        ImGui::BeginChild("nearbyDetails",ImVec2(0,0));
        if(o.isSelf)ImGui::Text("自身角色 · 地图 %s · 子区域 %s · 频道 %s",value(o,"map_id").c_str(),value(o,"subzone_id").c_str(),value(o,"channel").c_str());
        if(o.isSelf && (o.values.contains("glide_cooldown_ms") || o.values.contains("sprint_cooldown_ms")))
            ImGui::Text("最近收到的冷却值：滑翔 %s ms · 冲刺 %s ms（非实时倒计时）",value(o,"glide_cooldown_ms").c_str(),value(o,"sprint_cooldown_ms").c_str());
        if(o.values.contains("rotate_target_degrees"))ImGui::Text("最近转向通知：目标 %s° · 原因 %s",value(o,"rotate_target_degrees").c_str(),value(o,"rotate_reason").c_str());
        if(o.values.contains("rotate_reported_current"))ImGui::Text("通知中的当前朝向：%s（单位待确认；非实时朝向）",value(o,"rotate_reported_current").c_str());
        if(o.values.contains("last_on_ground_z"))ImGui::Text("最近地面高度 Z：%s（飞行上限基准；单位待确认）",value(o,"last_on_ground_z").c_str());
        if(o.values.contains("local_vessel_key")){
            ImGui::Text("局部坐标载体：%s；世界坐标等待换算",value(o,"local_vessel_key").c_str());
            ImGui::Text("局部位置：%s / %s / %s；局部朝向：%s",value(o,"local_x").c_str(),value(o,"local_y").c_str(),value(o,"local_z").c_str(),value(o,"local_yaw").c_str());
            if(o.values.contains("local_delta_x") || o.values.contains("local_delta_y") || o.values.contains("local_delta_z"))
                ImGui::Text("本次局部增量：%s / %s / %s",value(o,"local_delta_x").c_str(),value(o,"local_delta_y").c_str(),value(o,"local_delta_z").c_str());
        }
        ImGui::TextWrapped("服务器 %s    种族 %s    性别 %s    征服者等级 %s    装备等级 %s    战斗力 %s",value(o,"server_id").c_str(),value(o,"race").c_str(),value(o,"gender").c_str(),value(o,"conqueror_level").c_str(),value(o,"equipment_level").c_str(),value(o,"combat_power").c_str());
        if(o.values.contains("character_dbid") && ImGui::Button("填入玩家资料查询")){
            auto server=o.values.find("server_id");
            snprintf(queryServer_,sizeof(queryServer_),"%s",server==o.values.end()?"":server->second.c_str());
            snprintf(queryDbid_,sizeof(queryDbid_),"%s",o.values.at("character_dbid").c_str());if(proxyStreamIds_.contains(nearbyConnection_))queryConnection_=size_t(proxyStreamIds_.at(nearbyConnection_));selectQueryTab_=true;
        }
        ImGui::TextWrapped("翅膀 %s / 等级 %s / 外观 %s    称号 %s    公会 ID %s    联盟 %s",value(o,"wing_id").c_str(),value(o,"wing_level").c_str(),value(o,"wing_skin_id").c_str(),value(o,"title_id").c_str(),value(o,"guild_id").c_str(),value(o,"union_name").c_str());
        skinView(o.skins,skinRace(value(o,"race")));
        ImGui::TextDisabled("职业名称映射待确认；不将模板 ID 直接当作职业。没有出现包的对象不推断类型或等级。");ImGui::EndChild();
    }}else ImGui::TextDisabled("选择对象查看服务器、战斗力、翅膀和公会等信息。");
}
bool App::canClose(){
    if(queryProxy_.busy()){queryProxy_.drain();selectQueryTab_=true;message_="代理仍在维持游戏连接，请关闭游戏连接后再退出。";return false;}
    return queryProxy_.stop();
}
void App::queryView(){
    ImGui::TextUnformatted("玩家资料查询 · 透明网络代理（实验功能）");
    ImGui::TextWrapped("独立接管新连接，自动处理握手和消息加密。无需调试器、无需导入会话状态、不注入 DLL。所有分析页面共用此代理的收发消息。");
    ImGui::TextWrapped("请在登录游戏前启用。仅接管目标进程指定端口的 IPv4 TCP；加速器隧道未验证。代理连接存在时，需先关闭游戏连接再退出工具。");
    ImGui::BeginDisabled(queryProxy_.running());ImGui::SetNextItemWidth(130);ImGui::InputInt("世界服务端口",&queryPort_);
    ImGui::SameLine();if(ImGui::Button("启用透明代理")){
        startCapture();
    }ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(!queryProxy_.running());
    if(ImGui::Button("停止接管新连接"))queryProxy_.drain();ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(!queryProxy_.running() || queryProxy_.busy());
    if(ImGui::Button("关闭代理"))queryProxy_.stop();ImGui::EndDisabled();
    ImGui::TextWrapped("%s",queryProxy_.status().c_str());
    auto connections=queryProxy_.connections();
    if(!queryProxy_.running()){connections.clear();for(const auto& [id,c]:recordedConnections_)connections.push_back(c);}
    if(ImGui::BeginTable("queryConnections",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,ImVec2(0,130))){
        for(const char* title:{"连接","游戏端","服务器","状态"})ImGui::TableSetupColumn(title);ImGui::TableHeadersRow();
        for(const auto& c:connections){ImGui::PushID(int(c.id));ImGui::TableNextRow();ImGui::TableNextColumn();
            if(ImGui::Selectable(std::to_string(c.id).c_str(),queryConnection_==c.id,ImGuiSelectableFlags_SpanAllColumns))queryConnection_=c.id;
            ImGui::TableNextColumn();ImGui::TextUnformatted(c.client.text().c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(c.remote.text().c_str());
            ImGui::TableNextColumn();ImGui::TextUnformatted(c.status.c_str());ImGui::PopID();
        }ImGui::EndTable();
    }
    std::erase_if(connections,[](const auto& c){return c.remote.port==13700;});
    if(connections.empty()){ImGui::TextDisabled("尚无代理连接：启用后需由游戏重新建立连接。");return;}
    auto selected=std::find_if(connections.begin(),connections.end(),[&](const auto& c){return c.id==queryConnection_;});
    if(selected==connections.end()){selected=connections.begin();queryConnection_=selected->id;}
    const auto& c=*selected;
    ImGui::TextWrapped("连接 %zu：%s",c.id,c.status.c_str());
    ImGui::TextDisabled("完成握手、核对出站编码并收到自身角色入场消息后，启用查询按钮。");
    ImGui::SetNextItemWidth(140);ImGui::InputText("服务器 ID",queryServer_,sizeof(queryServer_),ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();ImGui::SetNextItemWidth(260);ImGui::InputText("角色数据库 ID",queryDbid_,sizeof(queryDbid_),ImGuiInputTextFlags_CharsDecimal);
    uint32_t server=0;uint64_t dbid=0;
    auto parse=[](const char* text,auto& number){auto end=text+strlen(text);auto result=std::from_chars(text,end,number);return text!=end && result.ec==std::errc{} && result.ptr==end;};
    bool valid=parse(queryServer_,server) && parse(queryDbid_,dbid) && server>0 && server<=65535 && dbid>0 && dbid<=uint64_t(INT64_MAX);
    ImGui::BeginDisabled(!valid || !c.open || !c.ready || c.waiting);
    if(ImGui::Button("查询一次"))if(!queryProxy_.request(c.id,server,dbid))message_="查询未排队：连接忙碌或尚未就绪。";
    ImGui::EndDisabled();ImGui::SameLine();ImGui::TextDisabled("串行查询；无固定间隔；超时不自动重发。");
    ImGui::TextWrapped("%s",c.response.status.empty()?"收到资料响应后显示角色字段、时装名称和染色。":c.response.status.c_str());
    ImGui::BeginChild("queryResultScroll",ImVec2(0,0));
    unsigned race=0;for(const auto& f:c.response.fields)if(f.name=="资料角色 种族")race=skinRace(f.value);
    const bool queryFailed=c.response.queryResult && *c.response.queryResult!=0;
    if(c.response.queryResult){ImGui::TextColored(queryFailed?ImVec4(1,.55f,.35f,1):ImVec4(.3f,.85f,.6f,1),"%s",gameQueryResultText(*c.response.queryResult).c_str());}
    if(c.response.skinsComplete && !queryFailed)skinView(c.response.skins,race);
    if(!c.response.fields.empty() && ImGui::CollapsingHeader("资料原始字段") && ImGui::BeginTable("queryResponse",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable)){
        ImGui::TableSetupColumn("字段");ImGui::TableSetupColumn("值");ImGui::TableSetupColumn("含义");ImGui::TableHeadersRow();
        for(const auto& f:c.response.fields){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(f.name.c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(f.value.c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(f.meaningKnown?"已确认":"待确认");}ImGui::EndTable();
    }
    ImGui::EndChild();
}
void App::guildView(){
    ImGui::TextUnformatted("军团查询 · 列表与名称搜索");
    ImGui::TextWrapped("复用当前世界代理连接。手动发送一次，不自动重试；未确认支持全服遍历或跨服查询。");
    auto connections=queryProxy_.connections();
    if(!queryProxy_.running()){connections.clear();for(const auto& [id,c]:recordedConnections_)connections.push_back(c);}
    std::erase_if(connections,[](const auto& c){return c.remote.port==13700;});
    if(connections.empty()){ImGui::TextDisabled("尚无世界代理连接。请先启用独立代理并重新登录游戏。");return;}
    auto selected=std::find_if(connections.begin(),connections.end(),[&](const auto& c){return c.id==guildConnection_;});
    if(selected==connections.end()){selected=std::find_if(connections.begin(),connections.end(),[](const auto& c){return c.open && c.ready;});if(selected==connections.end())selected=connections.begin();guildConnection_=selected->id;}
    auto label=[](const QueryConnection& c){return std::to_string(c.id)+"  "+c.remote.text()+(c.open?"":"（已断开）");};
    if(ImGui::BeginCombo("游戏连接##guild",label(*selected).c_str())){
        for(const auto& c:connections)if(ImGui::Selectable(label(c).c_str(),guildConnection_==c.id)){guildConnection_=c.id;guildNotice_.clear();}
        ImGui::EndCombo();
    }
    selected=std::find_if(connections.begin(),connections.end(),[&](const auto& c){return c.id==guildConnection_;});const auto& c=*selected;
    ImGui::TextWrapped("连接状态：%s",c.status.c_str());
    ImGui::SetNextItemWidth(180);ImGui::Combo("列表排序",&guildOrder_,"按人数\0按排名\0");
    ImGui::SameLine();ImGui::BeginDisabled(!c.queryAvailable);
    if(ImGui::Button("获取军团列表"))guildNotice_=queryProxy_.requestGuild(c.id,false,uint8_t(guildOrder_),{})?"列表请求已排队":"未排队：连接忙碌或尚未就绪";
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(320);ImGui::InputText("军团名称",guildName_,sizeof(guildName_));ImGui::SameLine();
    ImGui::BeginDisabled(!c.queryAvailable || !guildName_[0]);
    if(ImGui::Button("搜索军团"))guildNotice_=queryProxy_.requestGuild(c.id,true,0,guildName_)?"搜索请求已排队":"未排队：连接忙碌或参数无效";
    ImGui::EndDisabled();
    if(!guildNotice_.empty())ImGui::TextUnformatted(guildNotice_.c_str());
    ImGui::TextWrapped("%s",c.guildStatus.c_str());
    ImGui::Text("已发送：%llu  收到响应：%llu  最近响应：0x%04X",(unsigned long long)c.guildSent,(unsigned long long)c.guildReceived,c.guildOpcode);
    ImGui::TextDisabled("同时显示游戏页面产生的响应；避免与游戏内军团搜索同时操作。只保留本连接最近一份结果。");
    const auto& m=c.guildResponse;if(m.name.empty())return;
    ImGui::Separator();ImGui::TextUnformatted(m.name.c_str());ImGui::TextWrapped("解析状态：%s",m.status.c_str());
    std::string result,count;
    for(const auto& f:m.fields){if(f.name=="结果码（含义待确认）")result=f.value;if(f.name=="公会数量")count=f.value;}
    ImGui::Text("结果码：%s（0 为成功）  返回数量：%s",result.c_str(),count.c_str());
    if(!m.structureComplete)ImGui::TextWrapped("响应未完整解析，列表可能不完整；请保留会话以便核对。");
    if(ImGui::BeginTabBar("guildResults")){
        if(ImGui::BeginTabItem("军团列表")){
            std::vector<std::map<std::string,std::string>> rows;
            for(const auto& f:m.fields){if(!f.name.starts_with("公会["))continue;auto end=f.name.find(']');if(end==std::string::npos)continue;
                unsigned index=0;auto parsed=std::from_chars(f.name.data()+7,f.name.data()+end,index);
                if(parsed.ec!=std::errc{} || index>10000)continue;
                if(rows.size()<=index)rows.resize(index+1);rows[index][f.name.substr(end+2)]=f.value;
            }
            if(ImGui::BeginTable("guildList",11,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY,ImVec2(0,0))){
                const char* titles[]={"军团名称","军团长","人数 / 上限","等级","简介","入会规则","已申请","阵营","军团 ID","徽章 ID","经验"};
                for(auto title:titles)ImGui::TableSetupColumn(title,ImGuiTableColumnFlags_WidthFixed,130);
                ImGui::TableSetupScrollFreeze(1,1);ImGui::TableHeadersRow();
                for(auto& row:rows){ImGui::TableNextRow();
                    std::string values[]={row["公会名称"],row["会长名称"],row["成员数"]+" / "+row["成员上限"],row["公会等级"],row["简介"],row["入会规则"],row["已申请加入"],row["公会种族"],row["公会 ID"],row["徽章 ID"],row["公会经验"]};
                    for(const auto& value:values){ImGui::TableNextColumn();ImGui::TextUnformatted(value.c_str());if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",value.c_str());}
                }ImGui::EndTable();
            }ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("响应字段")){
            if(ImGui::BeginTable("guildFields",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,ImVec2(0,0))){
                for(auto title:{"字段","值","偏移 / 长度"})ImGui::TableSetupColumn(title);ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
                for(const auto& f:m.fields){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(f.name.c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(f.value.c_str());ImGui::TableNextColumn();ImGui::Text("%zu / %zu",f.offset,f.size);}ImGui::EndTable();
            }ImGui::EndTabItem();
        }ImGui::EndTabBar();
    }
}
void App::mailDebugView(const QueryConnection* c){
    if(!ImGui::CollapsingHeader("邮件测试",ImGuiTreeNodeFlags_DefaultOpen))return;
    ImGui::SetNextItemWidth(220);
    if(ImGui::Combo("邮件类型",&mailType_,"个人（1）\0军团（2）\0")){mailPreview_.clear();mailNotice_.clear();}
    if(mailType_==0)ImGui::TextWrapped("个人邮件：填写收件角色名。游戏页面显示每封 500 金币、每天最多 20 封；实际限制以服务器响应为准。");
    else ImGui::TextWrapped("军团邮件（实验）：类型值为 2。收件字段允许留空；目标范围、权限和费用尚未实测确认，以游戏规则及服务器响应为准。");
    ImGui::SetNextItemWidth(500);ImGui::InputText(mailType_==0?"收件人":"收件字段（可留空）",mailReceiver_,sizeof(mailReceiver_));
    ImGui::SetNextItemWidth(500);ImGui::InputText("标题（最多 50 字）",mailTitle_,sizeof(mailTitle_));
    ImGui::InputTextMultiline("正文（最多 500 字）",mailBody_,sizeof(mailBody_),ImVec2(500,110));
    MailRequest request{mailReceiver_,mailTitle_,mailBody_,uint8_t(mailType_+1)};std::string error;
    try{encodeMailRequest(request);}catch(const std::exception& e){error=e.what();}
    ImGui::BeginDisabled(!error.empty());
    if(ImGui::Button("构造明文包（不发送）"))mailPreview_=hex(encodeMailRequest(request));
    ImGui::EndDisabled();
    if(!mailPreview_.empty()){
        ImGui::SameLine();if(ImGui::Button("复制上次构造的包"))ImGui::SetClipboardText(mailPreview_.c_str());
        if(ImGui::TreeNode("上次构造的明文包")){ImGui::TextWrapped("%s",mailPreview_.c_str());ImGui::TreePop();}
    }
    const bool available=c && c->open && c->ready && !c->waiting && !c->guildPending && !c->jumpPending && !c->jumpRepeat &&
        !c->mail.pending && !c->mail.blocked && !c->mail.nativePending;
    ImGui::BeginDisabled(!available || !error.empty());
    if(ImGui::Button("发送一封测试邮件（消耗金币）"))
        mailNotice_=queryProxy_.requestMail(c->id,request)?"已排队；只发送一次，等待服务器确认":"未排队：连接状态已变化";
    ImGui::EndDisabled();
    if(!error.empty())ImGui::TextDisabled("%s",error.c_str());
    if(!mailNotice_.empty())ImGui::TextWrapped("%s",mailNotice_.c_str());
    if(c){
        ImGui::TextWrapped("邮件状态：%s",c->mail.status.c_str());
        ImGui::Text("本连接提交次数：%llu",static_cast<unsigned long long>(c->mail.sent));
        if(!c->mail.response.fields.empty() && ImGui::TreeNode("最近邮件响应字段")){
            for(const auto& field:c->mail.response.fields)ImGui::TextWrapped("%s：%s",field.name.c_str(),field.value.c_str());
            ImGui::TreePop();
        }
    }
    ImGui::TextDisabled("15 秒超时后结果为未知，不重发；发送期间请勿同时在游戏内发邮件。请求正文可在数据包分析中查看。");
}
void App::debugView(){
    ImGui::TextUnformatted("调试 · 使用当前世界代理连接");
    auto connections=queryProxy_.connections();std::erase_if(connections,[](const auto& c){return !c.open || c.remote.port==13700;});
    if(connections.empty()){
        mailDebugView(nullptr);
        ImGui::TextDisabled("没有正在运行的世界代理连接。离线文件不能发送动作。");
        ImGui::BeginDisabled();ImGui::Button("跳跃一次（完整序列）");ImGui::EndDisabled();return;
    }
    auto selected=std::find_if(connections.begin(),connections.end(),[&](const auto& c){return c.id==debugConnection_;});
    if(selected==connections.end()){selected=connections.begin();debugConnection_=selected->id;}
    auto label=[](const QueryConnection& c){return std::to_string(c.id)+"  "+c.remote.text();};
    if(ImGui::BeginCombo("游戏连接",label(*selected).c_str())){
        for(const auto& c:connections)if(ImGui::Selectable(label(c).c_str(),debugConnection_==c.id))debugConnection_=c.id;
        ImGui::EndCombo();
    }
    selected=std::find_if(connections.begin(),connections.end(),[&](const auto& c){return c.id==debugConnection_;});
    const auto& c=*selected;const auto& jump=c.jumpState;
    ImGui::TextWrapped("连接状态：%s",c.status.c_str());
    mailDebugView(&c);
    ImGui::Separator();
    ImGui::TextUnformatted("跳跃 · 完整序列与重复发送（实验）");
    ImGui::TextWrapped("在平地停稳后即可发送，无需先手动跳跃；若还没有停止位置，走一步再停下即可。纯网络发送不会直接触发本机动画。");
    ImGui::TextWrapped("动作状态：%s",jump.status.c_str());
    if(jump.grounded){ImGui::Text("最近地面位置：%.3f  %.3f  %.3f",jump.request.position[0],jump.request.position[1],jump.request.position[2]);
        ImGui::Text("位置距今：%.2f 秒；朝向：%.3f",double(jump.ageMs)/1000,jump.request.rotation);}
    if(jump.simulated)ImGui::Text("独立平地模型：%llu 条消息，%.3f 秒；无需学习轨迹",static_cast<unsigned long long>(jump.trajectoryFrames),double(jump.trajectoryDurationMs)/1000);
    if(jump.learned)ImGui::Text("已记录轨迹：%llu 条消息，%.2f 秒；起跳速度：%.3f",static_cast<unsigned long long>(jump.trajectoryFrames),double(jump.trajectoryDurationMs)/1000,jump.request.velocity[2]);
    if(jump.stopPositionUnconfirmed)ImGui::TextWrapped("已收到停止消息，但停止位置尚未解析；显示的是最后一次地面位置上报。");
    ImGui::BeginDisabled(!c.ready || !jump.ready || c.jumpPending || c.waiting || c.jumpRepeat || c.guildPending || c.mail.pending);
    if(ImGui::Button("跳跃一次（完整序列）"))if(!queryProxy_.requestJump(c.id))message_="起跳未排队：连接或动作状态已变化，请查看调试页。";
    ImGui::EndDisabled();ImGui::SameLine();ImGui::TextDisabled("至少间隔 5 秒；超时不重发。");
    static int repeatIntervalSeconds=5;
    ImGui::BeginDisabled(c.jumpRepeat);ImGui::SetNextItemWidth(120);
    ImGui::InputInt("重复间隔（秒）",&repeatIntervalSeconds);repeatIntervalSeconds=std::clamp(repeatIntervalSeconds,5,3600);
    ImGui::EndDisabled();
    if(c.jumpRepeat){
        if(ImGui::Button("停止重复（本次完成后）"))queryProxy_.setJumpRepeat(c.id,false);
        ImGui::SameLine();ImGui::Text("每 %u 秒一次；距下次 %.1f 秒",c.jumpIntervalSeconds,double(c.jumpNextInMs)/1000);
    }else{
        ImGui::BeginDisabled(!c.ready || !jump.ready || c.jumpPending || c.waiting || c.guildPending || c.mail.pending);
        if(ImGui::Button("开始重复跳跃"))if(!queryProxy_.setJumpRepeat(c.id,true,uint32_t(repeatIntervalSeconds)))message_="重复跳跃未启动，请确认位置与连接状态。";
        ImGui::EndDisabled();
    }
    ImGui::TextWrapped("开启后复用原地位置并更新发送时间；手动移动、切图、服务器校正或移动惩罚会停止重复。断线后不自动恢复。");
    ImGui::Text("本连接已开始发送：%llu 次",static_cast<unsigned long long>(c.jumpsSent));
    ImGui::TextWrapped("发送结果：%s",c.jumpStatus.c_str());
    ImGui::TextWrapped("服务器运动反馈：%s",jump.serverFeedback.c_str());
    if(!jump.serverStateFeedback.empty())ImGui::TextWrapped("服务器状态反馈：%s",jump.serverStateFeedback.c_str());
    ImGui::TextDisabled("日志记录每条发送进度和自身服务器反馈；提交完成不等于服务器接受。");
}
void App::reportView(){
    if(!reporter_){
        try {reporter_=std::make_unique<CharacterReporter>(CharacterReporter::defaultDirectory());auto cfg=reporter_->config();
            strncpy_s(reportUrl_,cfg.url.c_str(),_TRUNCATE);strncpy_s(reportToken_,cfg.token.c_str(),_TRUNCATE);
        }catch(const std::exception& e){ImGui::TextWrapped("无法初始化角色上报：%s",e.what());return;}
    }
    ImGui::TextUnformatted("角色资料上报 · 本地持久去重");
    ImGui::TextWrapped("首次发现或资料变化后加入队列，每 60 秒自动上传一批，最多 200 名玩家。仅采集实时游戏连接，每次启动需手动启用。");
    auto status=reporter_->status();
    ImGui::SetNextItemWidth(560);ImGui::InputText("控制台地址",reportUrl_,sizeof(reportUrl_));
    ImGui::SetNextItemWidth(560);ImGui::InputText("上传令牌",reportToken_,sizeof(reportToken_),ImGuiInputTextFlags_Password);
    ImGui::TextUnformatted("仅上报周围玩家（不包含自身、NPC 和怪物）");
    auto configure=[&](bool enabled){reporter_->configure({reportUrl_,reportToken_},enabled);};
    if(ImGui::Button("开始上报"))configure(true);ImGui::SameLine();
    if(ImGui::Button("暂停上报")){auto cfg=reporter_->config();reporter_->configure(cfg,false);}ImGui::SameLine();
    ImGui::BeginDisabled(!status.enabled);
    if(ImGui::Button("立即上传"))reporter_->flush();ImGui::SameLine();
    if(ImGui::Button("重试失败"))reporter_->flush(true);ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::Text("状态：%s%s",status.enabled?"已启用":"已暂停",status.uploading?" / 上传中":status.authPaused?" / 令牌待检查":"");
    ImGui::Text("待上传：%zu    被拒绝：%zu    本次已确认：%zu    去重跳过：%zu",status.pending,status.blocked,status.success,status.deduplicated);
    if(status.lastSuccess){auto seconds=time_t(status.lastSuccess/1000);tm local{};localtime_s(&local,&seconds);char text[64]{};strftime(text,sizeof(text),"%Y-%m-%d %H:%M:%S",&local);ImGui::Text("最近成功：%s",text);}
    if(status.overflow)ImGui::Text("输入队列达到上限：%zu 次；仍可见角色将在后续扫描重试",status.overflow);
    if(status.cacheFull)ImGui::Text("本地缓存已满，新增玩家暂未保存：%zu 次；已缓存玩家仍会更新和上传",status.cacheFull);
    if(status.storageBlocked)ImGui::TextUnformatted("本地存储暂不可用：未保存数据仍在内存，关闭程序会丢失这部分数据。请先解除文件占用或释放磁盘空间。");
    ImGui::TextWrapped("%s",status.message.c_str());
    ImGui::TextDisabled("按服务器 ID + 角色数据库 ID 去重；坐标和 HP 变化不会触发上传。");
    ImGui::TextWrapped("使用现有 /api/characters/upload 接口；令牌在本机加密保存。部分未知字段可能按网页原接口规则覆盖为空或 0。");
    auto path=CharacterReporter::defaultDirectory().u8string();ImGui::TextWrapped("本地数据：%s",reinterpret_cast<const char*>(path.c_str()));
}
void App::queryWorkerView(){
    ImGui::TextUnformatted("控制台在线查询服务");
    ImGui::TextWrapped("启用后，网页控制台只会把与本机当前游戏区服一致的玩家在线查询分配给本机。每包最多 50 人，本机逐个连续查询，整包完成后一次回传；每包限时 100 秒。切换角色或区服后会自动重新识别。");
    if(!queryWorker_){
        try{
            queryWorker_=std::make_unique<QueryWorker>(queryProxy_,CharacterReporter::defaultDirectory().parent_path()/L"query-worker");
            auto config=queryWorker_->config();snprintf(queryWorkerId_,sizeof(queryWorkerId_),"%s",config.clientId.c_str());
        }catch(const std::exception& e){ImGui::TextWrapped("查询服务初始化失败：%s",e.what());return;}
    }
    const auto status=queryWorker_->status();
    ImGui::TextUnformatted("连接：EMQX 加密连接；每次启动程序后需手动启用。");
    ImGui::BeginDisabled(status.enabled);
    ImGui::SetNextItemWidth(420);ImGui::InputText("客户端标识",queryWorkerId_,sizeof(queryWorkerId_),ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();if(ImGui::Button("复制标识"))ImGui::SetClipboardText(queryWorkerId_);
    ImGui::TextWrapped("查询凭据已内置，无需填写密码。");
    if(ImGui::Button("启用查询服务")){
        try{queryWorker_->start({queryWorkerId_,{}});message_="已启动控制台查询服务";}
        catch(const std::exception& e){message_=e.what();}
    }
    ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(!status.enabled);
    if(ImGui::Button("停止查询服务"))queryWorker_->stop();ImGui::EndDisabled();
    ImGui::Separator();ImGui::TextWrapped("状态：%s",status.message.c_str());
    ImGui::Text("通信：%s　可接任务：%s　执行中：%s",status.connected?"已连接":"未连接",status.ready?"是":"否",status.busy?"是":"否");
    ImGui::Text("当前游戏区服：%s",status.serverId.empty()?"尚未识别":status.serverId.c_str());
    ImGui::Text("已完成：%llu　未确认或失败：%llu　重复已去重：%llu　重连次数：%llu",status.completed,status.failed,status.duplicates,status.reconnects);
    ImGui::Text("已拒绝失效任务：%llu",status.rejected);
    if(!status.lastError.empty()){
        std::string reason=status.lastError;
        constexpr std::string_view prefix="game_query_failed:";
        if(reason.starts_with(prefix)){unsigned code=0;auto tail=std::string_view(reason).substr(prefix.size());auto parsed=std::from_chars(tail.data(),tail.data()+tail.size(),code);
            if(parsed.ec==std::errc{} && parsed.ptr==tail.data()+tail.size() && code<=65535)reason=gameQueryResultText(uint16_t(code));}
        else if(reason=="game_response_timeout")reason="游戏资料响应超时；此连接停止新查询，请重新登录游戏";
        ImGui::TextWrapped("最近失败：%s",reason.c_str());
    }
    if(!status.ready)for(const auto& connection:queryProxy_.connections())if(connection.open)ImGui::TextWrapped("游戏连接 %zu：%s",connection.id,connection.status.c_str());
    if(!status.target.empty())ImGui::Text("当前目标：%s",status.target.c_str());
    ImGui::TextWrapped("只有明确的游戏响应会判定在线或离线；超时、切换连接和游戏手动查询冲突均返回未确认。");
}
void App::chatBridgeView(){
    const auto status=chatBridge_.status();
    ImGui::TextUnformatted("聊天与远程控制");
    ImGui::TextWrapped("由 Aion2Pipe 自动管理 PlayNC HTTPS/WSS、聊天解析、私聊发送、MQTT 回执和换号控制；无需再单独启动 aion2-client。");
    ImGui::Text("组件：%s",status.installed?"已安装":"未安装");
    ImGui::Text("状态：%s",status.running?"运行中":"已停止");
    if(status.processId)ImGui::Text("进程：%lu",status.processId);
    ImGui::TextWrapped("%s",status.message.c_str());
    if(chatAutoStart_ && !status.running)ImGui::TextDisabled("组件意外退出，正在自动重启并等待 AION2.exe。");
    ImGui::BeginDisabled(status.running);if(ImGui::Button("启动聊天接管")){chatAutoStart_=false;chatBridge_.start();}ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!status.running);if(ImGui::Button("停止聊天接管")){chatAutoStart_=false;chatBridge_.stop();}ImGui::EndDisabled();
    ImGui::Separator();ImGui::TextDisabled("虚拟网卡模式：登录、世界及聊天外连统一沿 Windows 当前系统路由发送。");
    if(!status.logTail.empty() && ImGui::CollapsingHeader("运行日志",ImGuiTreeNodeFlags_DefaultOpen)){
        ImGui::BeginChild("chatBridgeLog",ImVec2(0,0),ImGuiChildFlags_Borders);ImGui::TextUnformatted(status.logTail.c_str());ImGui::EndChild();
    }
}
void App::pump(){
    add(queryProxy_.takePackets());
    chatBridge_.syncGameServer(queryProxy_);
    if(chatAutoStart_ && !chatBridge_.status().running){
        chatBridge_.start();
        diagnostics().write("chat_bridge_restart","Chat capture restarted; waiting for AION2.exe");
    }
    if(reporter_ && queryProxy_.running() && !queryProxy_.stats().queueDropped && GetTickCount64()-reportTick_>=1000){
        reportTick_=GetTickCount64();
        for(const auto& [key,model]:nearby_)if(!model.closed && !model.gap && key.starts_with("proxy:"))
            for(const auto& [id,object]:model.objects())reporter_->observe(object);
    }
    if(queryProxy_.running() && queryProxy_.stats().queueDropped){limit_=true;message_="分析队列已溢出，记录已停止；代理仍在转发。对象信息为历史快照。";}
    diagnosticSnapshot();
}
void App::draw() {
    pump(); auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Aion2Pipe",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    toolbar(); ImGui::Spacing();
    if(ImGui::BeginTabBar("pages")) {
    if(ImGui::BeginTabItem("周围对象",nullptr,selectNearbyTab_?ImGuiTabItemFlags_SetSelected:0)){nearbyView();ImGui::EndTabItem();}
    selectNearbyTab_=false;
    if(ImGui::BeginTabItem("玩家资料查询",nullptr,selectQueryTab_?ImGuiTabItemFlags_SetSelected:0)){queryView();ImGui::EndTabItem();}selectQueryTab_=false;
    if(ImGui::BeginTabItem("调试",nullptr,selectDebugTab_?ImGuiTabItemFlags_SetSelected:0)){debugView();ImGui::EndTabItem();}selectDebugTab_=false;
    if(ImGui::BeginTabItem("角色上报",nullptr,selectReportTab_?ImGuiTabItemFlags_SetSelected:0)){reportView();ImGui::EndTabItem();}selectReportTab_=false;
    if(ImGui::BeginTabItem("军团查询",nullptr,selectGuildTab_?ImGuiTabItemFlags_SetSelected:0)){selectGuildTab_=false;guildView();ImGui::EndTabItem();}
    if(ImGui::BeginTabItem("查询服务")){queryWorkerView();ImGui::EndTabItem();}
    if(ImGui::BeginTabItem("聊天控制")){chatBridgeView();ImGui::EndTabItem();}
    if(ImGui::BeginTabItem("数据包分析",nullptr,(selectGameTab_||selectFrameTab_)?ImGuiTabItemFlags_SetSelected:0)) {
    float height=ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("packetsPanel",ImVec2(0,height*packetPanelRatio_),ImGuiChildFlags_Borders); packetsView(); ImGui::EndChild();
    ImGui::InvisibleButton("调整面板高度",ImVec2(-1,5));
    if(ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if(ImGui::IsItemActive() && height>0) packetPanelRatio_=std::clamp(packetPanelRatio_+ImGui::GetIO().MouseDelta.y/height,.20f,.70f);
    ImGui::BeginChild("inspectorPanel",ImVec2(0,0),ImGuiChildFlags_Borders); inspector(); ImGui::EndChild();
    ImGui::EndTabItem();}
    ImGui::EndTabBar();}
    ImGui::End();
}
}
