#include "query_proxy.hpp"
#include "diagnostics.hpp"
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <stdexcept>
namespace aion {
namespace {
void put16(Bytes& b,size_t at,uint16_t v){b[at]=uint8_t(v>>8);b[at+1]=uint8_t(v);}
Endpoint endpoint(sockaddr_in a){Endpoint e;e.address[10]=e.address[11]=255;std::memcpy(e.address.data()+12,&a.sin_addr,4);e.port=ntohs(a.sin_port);return e;}
sockaddr_in address(const Endpoint& e){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(e.port);std::memcpy(&a.sin_addr,e.address.data()+12,4);return a;}
void sendAll(SOCKET s,std::span<const uint8_t> b){while(!b.empty()){int n=send(s,reinterpret_cast<const char*>(b.data()),int(std::min<size_t>(b.size(),65536)),0);if(n<=0)throw std::runtime_error("网络发送失败 "+std::to_string(WSAGetLastError()));b=b.subspan(size_t(n));}}
void configure(SOCKET s){DWORD timeout=5000;setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));BOOL on=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&on),sizeof(on));}
short proxyPriority(){
    wchar_t value[32]{};
    const auto size=GetEnvironmentVariableW(L"AION2PIPE_WINDIVERT_PRIORITY",value,DWORD(std::size(value)));
    if(!size || size>=std::size(value))return defaultProxyPriority;
    wchar_t* end=nullptr;const auto parsed=std::wcstol(value,&end,10);
    if(end==value || *end || parsed<WINDIVERT_PRIORITY_LOWEST || parsed>WINDIVERT_PRIORITY_HIGHEST)return defaultProxyPriority;
    return short(parsed);
}
}
bool rewriteQueryRoute(Bytes& raw,bool& outbound,const QueryRoute& r,uint16_t listener,uint16_t alt){
    if(r.alternatePort)alt=r.alternatePort;
    Packet p;std::string error;if(!parsePacket(raw,p,error) || p.protocol!=6 || p.ipv6)return false;
    const size_t tcp=(raw[0]&15)*4;
    auto reflect=[&](){for(unsigned i=0;i<4;++i)std::swap(raw[12+i],raw[16+i]);outbound=false;};
    if(outbound && p.source==r.client && p.destination==r.remote){put16(raw,tcp+2,listener);reflect();return true;}
    Endpoint reflectedSource=r.client;reflectedSource.port=listener;Endpoint reflectedDest=r.remote;reflectedDest.port=r.client.port;
    if(outbound && p.source==reflectedSource && p.destination==reflectedDest){put16(raw,tcp,r.remote.port);reflect();return true;}
    Endpoint alternate=r.remote;alternate.port=alt;
    if(r.upstream.port && p.source==r.upstream && p.destination==alternate){put16(raw,tcp+2,r.remote.port);return true;}
    if(r.upstream.port && p.source==r.remote && p.destination==r.upstream){put16(raw,tcp,alt);return true;}
    return false;
}
struct QueryProxy::Session {
    HANDLE ownerProcess=nullptr;
    ~Session(){if(ownerProcess)CloseHandle(ownerProcess);}
    QueryRoute route;QueryConnection view;
    uint32_t observedSequence[2]{};
    std::mutex mutex;std::thread worker;
    std::optional<std::pair<uint32_t,uint64_t>> command;
    std::shared_ptr<QueryReceipt> receipt;
    struct GuildCommand {bool search;uint8_t order;std::string name;};
    std::optional<GuildCommand> guildCommand;
    uint64_t guildQueuedAt{},guildSentAt{};uint16_t guildExpected{};
    bool guildConflict{};
    MailExchange mail;
    bool repeatArm{};uint64_t repeatRevision{},nextJumpAt{};
    bool jumpCommand{};uint64_t jumpQueuedAt{},lastJumpSent{};
    uint64_t queuedAt{},lastSent{},createdAt{};
    bool https{},accepted{},retired{};uint64_t closedAt{};
    SOCKET client=INVALID_SOCKET,remote=INVALID_SOCKET;
};
QueryProxy::~QueryProxy(){
    if(stop())return;
    // Normal window close is guarded. An unrecoverable UI shutdown must still
    // release threads/sockets, instead of terminating with joinable threads.
    diagnostics().write("query_shutdown","Unexpected shutdown: closing proxied connections");
    acceptNew_=running_=false;
    if(accept_.joinable())accept_.join();
    {std::lock_guard lock(mutex_);for(auto& s:sessions_){std::lock_guard sl(s->mutex);
        if(!s->accepted){s->accepted=true;--active_;}
        if(s->client!=INVALID_SOCKET)shutdown(s->client,SD_BOTH);
        if(s->remote!=INVALID_SOCKET)shutdown(s->remote,SD_BOTH);
    }}
    for(auto& s:sessions_)if(s->worker.joinable())s->worker.join();
    stop();
}
void QueryProxy::setStatus(std::string text){diagnostics().write("query_proxy",text);std::lock_guard lock(mutex_);status_=std::move(text);}
std::string QueryProxy::status()const{std::lock_guard lock(mutex_);return status_;}
std::string QueryProxy::routingMode()const{std::lock_guard lock(mutex_);return routingMode_;}
std::vector<QueryConnection> QueryProxy::connections()const{
    std::vector<QueryConnection> v;std::lock_guard lock(mutex_);
    for(const auto& s:sessions_){std::lock_guard sl(s->mutex);auto view=s->view;
        view.mail=s->mail.view;
        view.queryCooldownMs=0; // No fixed delay; waiting/command gates preserve response correlation.
        view.queryAvailable=view.open && view.ready && view.serverId && !view.mail.pending && !view.waiting && !view.guildPending && !s->command && !s->jumpCommand && !view.jumpPending && !view.jumpRepeat && !view.queryCooldownMs;
        v.push_back(std::move(view));
    }return v;
}
ProxyStats QueryProxy::stats()const{auto views=connections();std::lock_guard lock(eventsMutex_);auto out=stats_;out.connections=views.size();out.processes=std::any_of(views.begin(),views.end(),[](const auto& c){return c.open;})?1:0;return out;}
std::vector<Packet> QueryProxy::takePackets(){
    std::lock_guard lock(eventsMutex_);std::vector<Packet> out;out.reserve(events_.size());
    while(!events_.empty()){out.push_back(std::move(events_.front()));events_.pop_front();}eventBytes_=0;return out;
}
void QueryProxy::publish(Session& s,const ObservedFrame& frame,uint8_t flags){
    Packet p;p.proxyConnection=s.view.id;p.pid=s.view.pid;p.protocol=6;p.attributed=true;p.outbound=frame.outbound;
    p.plaintext=frame.plaintext;p.toolGenerated=frame.toolGenerated;p.flags=flags;
    p.source=frame.outbound?s.route.client:s.route.remote;p.destination=frame.outbound?s.route.remote:s.route.client;
    p.sequence=s.observedSequence[frame.outbound?0:1];s.observedSequence[frame.outbound?0:1]+=uint32_t(frame.plain.size())+((flags&2)?1:0)+((flags&1)?1:0);
    p.timeUs=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    p.raw=frame.wire;p.payload=frame.plain;
    enqueue(std::move(p));
}
void QueryProxy::publishWire(const Session& s,Packet p){
    p.proxyConnection=s.view.id;p.pid=s.view.pid;p.attributed=true;p.wireObservation=true;
    // Exact pre-rewrite IP bytes on both TCP legs. These are never fed to the
    // object model and never relabelled as a synthetic end-to-end TCP stream.
    p.outbound=p.source==s.route.client || p.source==s.route.upstream;
    p.timeUs=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    enqueue(std::move(p));
}
void QueryProxy::enqueue(Packet p){
    std::lock_guard lock(eventsMutex_);++stats_.received;
    auto cost=p.raw.size()+p.payload.size();
    // Stop observing on loss. Never silently join non-contiguous game data, and
    // never stop forwarding because the analysis UI is slow or its queue is full.
    if(stats_.queueDropped || events_.size()>=8192 || eventBytes_+cost>32*1024*1024){if(!stats_.queueDropped)diagnostics().write("proxy_observation_limit","Analysis queue full; observation stopped, forwarding continues");++stats_.queueDropped;return;}
    eventBytes_+=cost;++stats_.matched;events_.push_back(std::move(p));
}
bool QueryProxy::start(std::wstring exe,uint16_t port){
    if(!stop())return false;
    if(exe.empty() || exe.find_first_of(L"/\\")!=std::wstring::npos || !port){setStatus("游戏进程名或服务器端口无效");return false;}
    executable_=std::move(exe);serverPort_=port;
    {std::lock_guard lock(mutex_);routingMode_="虚拟网卡/系统路由模式";}
    {std::lock_guard lock(eventsMutex_);events_.clear();eventBytes_=0;stats_={};}
    listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(listener_==INVALID_SOCKET){setStatus("无法创建本地监听 socket");return false;}
    BOOL exclusive=TRUE;setsockopt(listener_,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
    sockaddr_in local{};local.sin_family=AF_INET;
    if(bind(listener_,reinterpret_cast<sockaddr*>(&local),sizeof(local)) || listen(listener_,8)){closesocket(listener_);listener_=INVALID_SOCKET;setStatus("无法启用本地查询转发");return false;}
    int length=sizeof(local);getsockname(listener_,reinterpret_cast<sockaddr*>(&local),&length);listenPort_=ntohs(local.sin_port);
    alternatePort_=serverPort_==65535?65534:uint16_t(serverPort_+1);if(alternatePort_==listenPort_)alternatePort_=serverPort_>2?uint16_t(serverPort_-1):65533;
    auto freeAlternate=[&](uint16_t candidate,uint16_t other){
        while(!candidate || candidate==serverPort_ || candidate==13700 || candidate==listenPort_ || candidate==other)++candidate;
        return candidate;
    };
    alternatePort_=freeAlternate(alternatePort_,0);loginAlternatePort_=freeAlternate(13699,alternatePort_);
    std::string filter="ip and tcp and (tcp.DstPort == 443 or tcp.DstPort == 13700 or tcp.SrcPort == 13700 or tcp.DstPort == "+std::to_string(loginAlternatePort_)+" or tcp.DstPort == "+std::to_string(serverPort_)+" or tcp.SrcPort == "+std::to_string(serverPort_)+" or tcp.SrcPort == "+std::to_string(listenPort_)+" or tcp.DstPort == "+std::to_string(alternatePort_)+")";
    const auto priority=proxyPriority();
    handle_=WinDivertOpen(filter.c_str(),WINDIVERT_LAYER_NETWORK,priority,0);
    if(handle_==INVALID_HANDLE_VALUE){closesocket(listener_);listener_=INVALID_SOCKET;setStatus("WinDivert 查询转发启动失败："+std::to_string(GetLastError()));return false;}
    WinDivertSetParam(handle_,WINDIVERT_PARAM_QUEUE_LENGTH,16384);WinDivertSetParam(handle_,WINDIVERT_PARAM_QUEUE_SIZE,32*1024*1024);
    running_=acceptNew_=forwarding_=true;
    try{network_=std::thread(&QueryProxy::networkLoop,this);accept_=std::thread(&QueryProxy::acceptLoop,this);}catch(...){running_=acceptNew_=false;WinDivertShutdown(handle_,WINDIVERT_SHUTDOWN_RECV);if(network_.joinable())network_.join();WinDivertClose(handle_);handle_=INVALID_HANDLE_VALUE;closesocket(listener_);listener_=INVALID_SOCKET;throw;}
    diagnostics().write("query_proxy_order","windivert_priority="+std::to_string(priority)+" route_mode=system");
    setStatus("已启用："+routingMode()+"；等待游戏新建 IPv4 登录/世界连接");return true;
}
void QueryProxy::drain(){acceptNew_=false;setStatus("已停止接管新连接；已有连接继续同步，关闭游戏连接后可退出");}
bool QueryProxy::stop(){
    {std::lock_guard lock(mutex_);if(busy())return false;acceptNew_=false;running_=false;}
    if(accept_.joinable())accept_.join();
    if(listener_!=INVALID_SOCKET){closesocket(listener_);listener_=INVALID_SOCKET;}
    if(handle_!=INVALID_HANDLE_VALUE)WinDivertShutdown(handle_,WINDIVERT_SHUTDOWN_RECV);
    if(network_.joinable())network_.join();
    if(handle_!=INVALID_HANDLE_VALUE){WinDivertClose(handle_);handle_=INVALID_HANDLE_VALUE;}
    for(auto& s:sessions_)if(s->worker.joinable())s->worker.join();
    sessions_.clear();{std::lock_guard lock(mutex_);status_="代理已关闭";}return true;
}
uint32_t QueryProxy::target(const Packet& p)const{
    DWORD size=0;if(GetExtendedTcpTable(nullptr,&size,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0)!=ERROR_INSUFFICIENT_BUFFER)return false;
    Bytes buffer(size);if(GetExtendedTcpTable(buffer.data(),&size,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0)!=NO_ERROR)return false;
    auto* table=reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for(DWORD i=0;i<table->dwNumEntries;++i){const auto& r=table->table[i];
        if(ntohs(u_short(r.dwLocalPort))!=p.source.port || ntohs(u_short(r.dwRemotePort))!=p.destination.port || std::memcmp(&r.dwLocalAddr,p.source.address.data()+12,4) || std::memcmp(&r.dwRemoteAddr,p.destination.address.data()+12,4))continue;
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.dwOwningPid);if(!process)return false;
        wchar_t name[32768]{};DWORD n=32768;bool ok=QueryFullProcessImageNameW(process,0,name,&n)!=FALSE;CloseHandle(process);
        auto basename=wcsrchr(name,L'\\');return ok && _wcsicmp(basename?basename+1:name,executable_.c_str())==0?r.dwOwningPid:0;
    }return false;
}
void QueryProxy::networkLoop(){
    Bytes buffer(65535);WINDIVERT_ADDRESS addr{};UINT length=0;
    while(WinDivertRecv(handle_,buffer.data(),UINT(buffer.size()),&length,&addr)){
        Bytes raw(buffer.begin(),buffer.begin()+length);Packet p;std::string error;bool out=addr.Outbound!=0,changed=false;
        if(parsePacket(raw,p,error)){
            std::lock_guard lock(mutex_);
            for(const auto& s:sessions_){std::lock_guard sl(s->mutex);
                const bool fresh=out && (p.flags&0x12)==2 && p.source==s->route.client && p.destination==s->route.remote && p.sequence+1!=s->route.firstSequence;
                if(fresh && s->accepted && !s->view.open)s->retired=true;
                if(!s->retired && rewriteQueryRoute(raw,out,s->route,listenPort_,alternatePort_)){publishWire(*s,p);changed=true;break;}}
            uint32_t owner=0;
            const bool candidate=!changed && running_ && acceptNew_ && out && (p.flags&0x12)==2 && active_.load()<32 && sessions_.size()<256;
            const bool direct=p.destination.port==serverPort_ || p.destination.port==13700 || p.destination.port==443;
            if(candidate && direct && (owner=target(p))){
                auto s=std::make_shared<Session>();s->https=p.destination.port==443;s->route={p.source,p.destination,{},p.sequence+1};s->route.alternatePort=p.destination.port==serverPort_?alternatePort_:loginAlternatePort_;
                s->view.id=++nextConnection_;s->view.pid=owner;s->view.client=p.source;s->view.remote=p.destination;s->view.firstSequence=p.sequence+1;s->view.status="等待连接建立";s->createdAt=GetTickCount64();
                s->ownerProcess=OpenProcess(SYNCHRONIZE,FALSE,owner);
                sessions_.push_back(s);++active_;publishWire(*s,p);changed=rewriteQueryRoute(raw,out,s->route,listenPort_,alternatePort_);
                diagnostics().write("query_route_created","connection="+std::to_string(s->view.id)+" route=system destination_port="+std::to_string(p.destination.port)+" server_port="+std::to_string(serverPort_));
            }
        }
        // Local-to-local traffic has only an outbound path in WinDivert.
        if(changed){addr.Outbound=out || addr.Loopback;WinDivertHelperCalcChecksums(raw.data(),UINT(raw.size()),&addr,0);}
        if(!WinDivertSend(handle_,raw.data(),UINT(raw.size()),nullptr,&addr)){
            // TCP retransmits a lost IP packet; never emit it on an untranslated path.
            diagnostics().write("query_forward_error","WinDivertSend="+std::to_string(GetLastError()));
        }
    }
    forwarding_=false;
    if(running_){acceptNew_=false;setStatus("网络转发已中断，代理连接将关闭；请停止代理后重新连接");}
}
void QueryProxy::acceptLoop(){
    while(running_){
        std::vector<std::shared_ptr<Session>> completed;
        {std::lock_guard lock(mutex_);const auto now=GetTickCount64();
            for(auto it=sessions_.begin();it!=sessions_.end();){auto s=*it;std::lock_guard sl(s->mutex);
                // A process handle identifies the original process even if its PID
                // is reused. Do not wait for a remote FIN after the game exits.
                if(s->ownerProcess && WaitForSingleObject(s->ownerProcess,0)==WAIT_OBJECT_0){
                    if(!s->closedAt)diagnostics().write("query_owner_exited","connection="+std::to_string(s->view.id));
                    if(!s->accepted){s->accepted=true;s->closedAt=now;--active_;}
                    if(s->client!=INVALID_SOCKET)shutdown(s->client,SD_BOTH);
                    if(s->remote!=INVALID_SOCKET)shutdown(s->remote,SD_BOTH);
                    CloseHandle(s->ownerProcess);s->ownerProcess=nullptr;
                }
                if(!s->accepted && now-s->createdAt>15000){s->accepted=true;s->closedAt=now;s->view.status="连接建立超时";--active_;}
                // Retain closed route translations for late FIN/ACK retransmits.
                // Never join a session thread while holding either mutex.
                if(s->closedAt && (s->retired || now-s->closedAt>=120000)){completed.push_back(s);it=sessions_.erase(it);}
                else ++it;
            }
        }
        for(auto& s:completed)if(s->worker.joinable())s->worker.join();
        fd_set fds;FD_ZERO(&fds);FD_SET(listener_,&fds);timeval tv{0,100000};int n=select(0,&fds,nullptr,nullptr,&tv);if(n<=0)continue;
        sockaddr_in peer{},local{};int length=sizeof(peer);SOCKET client=accept(listener_,reinterpret_cast<sockaddr*>(&peer),&length);if(client==INVALID_SOCKET)continue;
        length=sizeof(local);getsockname(client,reinterpret_cast<sockaddr*>(&local),&length);
        std::shared_ptr<Session> found;
        {std::lock_guard lock(mutex_);for(auto& s:sessions_){std::lock_guard sl(s->mutex);auto source=endpoint(peer),dest=endpoint(local);
            if(running_ && forwarding_ && !s->accepted && source.address==s->route.remote.address && source.port==s->route.client.port && dest.address==s->route.client.address){s->accepted=true;s->view.open=true;s->client=client;found=s;break;}}}
        if(!found){closesocket(client);continue;}
        try{found->worker=std::thread(&QueryProxy::sessionLoop,this,found,client);}catch(...){std::lock_guard lock(found->mutex);closesocket(client);found->client=INVALID_SOCKET;found->view.open=false;found->view.status="无法启动转发线程";--active_;}
    }
}
bool QueryProxy::request(size_t id,uint32_t server,uint64_t dbid){
    return bool(requestTracked(id,server,dbid));
}
std::shared_ptr<QueryReceipt> QueryProxy::requestTracked(size_t id,uint32_t server,uint64_t dbid){
    try{encodeViewCharRequest(server,dbid);}catch(...){return {};}
    std::lock_guard lock(mutex_);for(auto& s:sessions_)if(s->view.id==id){std::lock_guard sl(s->mutex);
        if(!s->view.open || !s->view.ready || !s->view.serverId || s->view.serverId!=server || s->mail.view.pending || s->view.guildPending || s->view.waiting || s->command || s->jumpCommand || s->view.jumpPending || s->view.jumpRepeat){
            if(s->view.serverId && s->view.serverId!=server){s->view.status="已拒绝跨区服查询：当前 "+std::to_string(s->view.serverId)+"，目标 "+std::to_string(server);diagnostics().write("query_server_mismatch","connection="+std::to_string(id)+" current="+std::to_string(s->view.serverId)+" target="+std::to_string(server));}
            return {};
        }
        s->receipt=std::make_shared<QueryReceipt>(server,dbid);
        s->command=std::pair(server,dbid);s->queuedAt=GetTickCount64();s->view.waiting=true;s->view.response={};s->view.status="查询已排队，等待完整帧边界";return s->receipt;
    }return {};
}
bool QueryProxy::requestMail(size_t id,const MailRequest& request){
    try{encodeMailRequest(request);}catch(...){return false;}
    std::lock_guard lock(mutex_);for(auto& s:sessions_)if(s->view.id==id){std::lock_guard sl(s->mutex);
        if(!s->view.open || !s->view.ready || s->https || s->mail.view.pending || s->view.guildPending || s->view.waiting || s->command || s->jumpCommand || s->view.jumpPending || s->view.jumpRepeat)return false;
        if(!s->mail.queue(request,GetTickCount64()))return false;
        diagnostics().write("mail_queued","connection="+std::to_string(id));return true;
    }return false;
}
bool QueryProxy::requestGuild(size_t id,bool search,uint8_t order,std::string name){
    try{encodeGuildRequest(search,order,name);}catch(...){return false;}
    std::lock_guard lock(mutex_);for(auto& s:sessions_)if(s->view.id==id){std::lock_guard sl(s->mutex);
        if(!s->view.open || !s->view.ready || s->mail.view.pending || s->view.guildPending || s->view.waiting || s->command || s->jumpCommand || s->view.jumpPending || s->view.jumpRepeat)return false;
        s->guildCommand=Session::GuildCommand{search,order,std::move(name)};
        s->guildQueuedAt=GetTickCount64();s->guildSentAt=0;s->guildExpected=search?0x8a09:0x8a07;s->guildConflict=false;
        s->view.guildPending=true;s->view.guildResponse={};s->view.guildOpcode=0;s->view.guildStatus="军团查询已排队";
        return true;
    }return false;
}
bool QueryProxy::requestJump(size_t id){
    std::lock_guard lock(mutex_);for(auto& s:sessions_)if(s->view.id==id){std::lock_guard sl(s->mutex);
        auto now=GetTickCount64();
        if(!s->view.open || !s->view.ready || s->mail.view.pending || s->view.guildPending || !s->view.jumpState.ready || s->view.jumpRepeat || s->view.waiting || s->command || s->jumpCommand || s->view.jumpPending || (s->lastJumpSent && now-s->lastJumpSent<5000))return false;
        s->jumpCommand=s->view.jumpPending=true;s->jumpQueuedAt=now;s->view.jumpStatus="完整跳跃已排队";
        diagnostics().write("jump_queued","connection="+std::to_string(id));return true;
    }return false;
}
bool QueryProxy::setJumpRepeat(size_t id,bool enabled,uint32_t intervalSeconds){
    if(enabled && (intervalSeconds<5 || intervalSeconds>3600))return false;
    std::lock_guard lock(mutex_);for(auto& s:sessions_)if(s->view.id==id){std::lock_guard sl(s->mutex);
        if(!enabled){
            s->view.jumpRepeat=s->repeatArm=false;s->view.jumpNextInMs=0;
            if(s->jumpCommand){s->jumpCommand=s->view.jumpPending=false;}
            s->view.jumpStatus=s->view.jumpPending?"已停止重复；正在发送的本次跳跃完成后结束":"已停止重复跳跃";
            diagnostics().write("jump_repeat_stopped","connection="+std::to_string(id));return true;
        }
        if(!s->view.open || !s->view.ready || s->mail.view.pending || s->view.guildPending || !s->view.jumpState.ready || s->view.waiting || s->view.jumpPending || s->view.jumpRepeat)return false;
        s->view.jumpRepeat=s->repeatArm=true;s->view.jumpIntervalSeconds=intervalSeconds;s->view.jumpNextInMs=0;
        s->view.jumpStatus="重复跳跃已开启，等待状态复核";
        diagnostics().write("jump_repeat_enabled","connection="+std::to_string(id)+" interval_seconds="+std::to_string(intervalSeconds));return true;
    }return false;
}
void QueryProxy::sessionLoop(std::shared_ptr<Session> s,SOCKET client){
    SOCKET remote=INVALID_SOCKET;WorldMitm cipher(s->route.client,s->route.remote,s->route.firstSequence);
    const bool login=s->route.remote.port==13700 && serverPort_!=13700;
    const bool opaque=login || s->https;
    Bytes loginPending[2];bool loginOpaque[2]{};
    bool clientEof=false,remoteEof=false,ours=false,native=false,ambiguous=false,disabled=false;
    uint64_t halfClosedAt=0;
    std::string lastCipherStatus;
    uint64_t waitingSince=0;
    JumpTracker jump;
    std::vector<JumpStep> jumpPlan;size_t jumpIndex=0;uint64_t jumpStartedAt=0,jumpRevision=0;
    auto flushJumpEvidence=[&](){for(const auto& [event,details]:jump.takeEvidence())diagnostics().write(event,"connection="+std::to_string(s->view.id)+" "+details);};
    publish(*s,{true,false,false,{},{}},2);publish(*s,{false,false,false,{},{}},0x12);
    // Observe responses independently of packet retention. Do not swallow native UI responses:
    // this protocol has no request ID, so the page explicitly displays the latest observed response.
    auto observeGuild=[&](auto&& self,std::span<const uint8_t> bytes,bool outbound,bool generated,unsigned depth)->void{
        if(depth>4)return;
        for(const auto& f:splitGameFrames(bytes).frames){
            auto frame=bytes.subspan(f.offset,f.length);auto op=uint16_t(readInteger(frame,f.prefixBytes,2,false));
            if(op==0xffff && !outbound){auto m=decodeGameFrame(frame,true);if(!m.expanded.empty())self(self,m.expanded,false,generated,depth+1);}
            else if(outbound && !generated && op==0xe201){
                std::lock_guard lock(s->mutex);s->mail.nativeRequest(GetTickCount64());
            }else if(!outbound && op==0xe202){
                auto m=decodeGameFrame(frame,true);std::lock_guard lock(s->mutex);
                diagnostics().write("mail_response","connection="+std::to_string(s->view.id)+" complete="+std::to_string(m.structureComplete)+" result="+(m.mailResult?std::to_string(*m.mailResult):"unknown"));
                s->mail.receive(std::move(m));
            }else if(outbound && !generated && (op==0x8a06 || op==0x8a08)){
                std::lock_guard lock(s->mutex);
                if(s->view.guildPending){s->guildConflict=true;s->view.guildStatus="游戏同时发起军团查询，响应归属无法确认";
                    if(s->guildCommand){s->guildCommand.reset();s->view.guildPending=false;s->view.guildStatus="未发送：游戏正在查询军团";}}
            }else if(!outbound && (op==0x8a07 || op==0x8a09)){
                auto m=decodeGameFrame(frame,true);std::lock_guard lock(s->mutex);
                s->view.guildResponse=std::move(m);s->view.guildOpcode=op;++s->view.guildReceived;
                if(s->view.guildPending && s->guildSentAt && op==s->guildExpected)s->view.guildPending=false;
                s->view.guildStatus=s->guildConflict?"收到军团响应；与游戏查询重叠，归属未确认":"收到军团响应（本连接最近一条）";
                diagnostics().write("guild_response","connection="+std::to_string(s->view.id)+" opcode="+std::to_string(op)+" complete="+std::to_string(s->view.guildResponse.structureComplete));
            }
        }
    };
    auto observeServer=[&](auto&& self,std::span<const uint8_t> bytes,unsigned depth)->void{
        if(depth>4)return;
        for(const auto& f:splitGameFrames(bytes).frames){auto frame=bytes.subspan(f.offset,f.length);const auto op=uint16_t(readInteger(frame,f.prefixBytes,2,false));
            if(op==0xffff){auto m=decodeGameFrame(frame,true);if(!m.expanded.empty())self(self,m.expanded,depth+1);}
            else if(op==0x3621){std::lock_guard lock(s->mutex);s->view.serverId=0;}
            else if(op==0x3633){auto m=decodeGameFrame(frame,true);if(auto server=selfServerId(m)){std::lock_guard lock(s->mutex);s->view.serverId=*server;diagnostics().write("query_game_server","connection="+std::to_string(s->view.id)+" server="+std::to_string(*server));}}
        }
    };
    auto observe=[&](){for(const auto& frame:cipher.observations){
        if(frame.plaintext && !frame.toolGenerated)jump.observe(frame.plain,frame.outbound,GetTickCount64());
        if(frame.plaintext && !frame.outbound && !frame.toolGenerated)observeServer(observeServer,frame.plain,0);
        if(frame.plaintext)observeGuild(observeGuild,frame.plain,frame.outbound,frame.toolGenerated,0);
        publish(*s,frame);
    }cipher.observations.clear();flushJumpEvidence();};
    auto message=[&](std::string text){std::lock_guard lock(s->mutex);s->view.status=std::move(text);};
    auto observeLogin=[&](std::span<const uint8_t> input,bool outbound){
        auto index=outbound?0:1;auto& pending=loginPending[index];
        if(loginOpaque[index]){publish(*s,{outbound,false,false,Bytes(input.begin(),input.end()),Bytes(input.begin(),input.end())});return;}
        pending.insert(pending.end(),input.begin(),input.end());auto frames=splitGameFrames(pending);
        if(frames.status.starts_with("无效") || frames.status.starts_with("帧超过") || pending.size()>4*1024*1024){
            loginOpaque[index]=true;for(size_t at=0;at<pending.size();at+=65536){auto n=std::min<size_t>(65536,pending.size()-at);Bytes b(pending.begin()+at,pending.begin()+at+n);publish(*s,{outbound,false,false,b,b});}pending.clear();return;
        }
        for(const auto& f:frames.frames){auto part=std::span(pending).subspan(f.offset,f.length);Bytes b(part.begin(),part.end());publish(*s,{outbound,!outbound,false,b,b});}
        pending.erase(pending.begin(),pending.begin()+frames.consumed);
    };
    auto examine=[&](auto&& self,std::span<const uint8_t> bytes,unsigned depth)->Bytes{
        if(depth>4)throw std::runtime_error("响应嵌套超过限制");auto split=splitGameFrames(bytes);Bytes forwarded;
        for(const auto& f:split.frames){auto frame=bytes.subspan(f.offset,f.length);auto opcode=readInteger(frame,f.prefixBytes,2,false);
            bool suppress=false;
            if(opcode==0xffff && (ours||native)){
                auto m=decodeGameFrame(frame,true);
                if(m.expanded.empty())throw std::runtime_error("资料响应压缩块无法展开");
                auto filtered=self(self,m.expanded,depth+1);
                if(filtered!=m.expanded){forwarded.insert(forwarded.end(),filtered.begin(),filtered.end());continue;}
            }
            else if(opcode==0x3650 && (ours||native)){
                auto m=decodeGameFrame(frame,true);std::lock_guard lock(s->mutex);
                if(ours){if(s->receipt)s->receipt->complete(m,ambiguous);s->view.response=std::move(m);s->view.status=ambiguous?"收到资料响应，但游戏同时发起查询，无法确认归属":s->view.response.queryResult && *s->view.response.queryResult!=0?s->view.response.status:"收到资料响应（按串行请求关联；装备尾部仍待解析）";
                    diagnostics().write("query_result","connection="+std::to_string(s->view.id)+" result="+(s->view.response.queryResult?std::to_string(*s->view.response.queryResult):"missing")+" prefix_complete="+std::to_string(s->view.response.viewCharPrefixComplete)+" ambiguous="+std::to_string(ambiguous)+(s->receipt?" target_server="+std::to_string(s->receipt->serverId)+" target_character="+std::to_string(s->receipt->characterId):""));}
                suppress=ours && !ambiguous;
                if(ambiguous){disabled=true;s->view.ready=false;}
                diagnostics().write("query_response","connection="+std::to_string(s->view.id)+" own="+std::to_string(ours)+" ambiguous="+std::to_string(ambiguous));
                s->view.waiting=false;ours=native=ambiguous=false;
            }
            if(!suppress)forwarded.insert(forwarded.end(),frame.begin(),frame.end());
        }
        return forwarded;
    };
    try{
        configure(client);
        Endpoint upstream=s->route.client;upstream.port=0;
        auto openRemote=[&](sockaddr_in dest,bool bindSource=true){
            auto socketHandle=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(socketHandle==INVALID_SOCKET)throw std::runtime_error("无法创建上游 socket");configure(socketHandle);
            if(bindSource){auto local=address(upstream);
                if(bind(socketHandle,reinterpret_cast<sockaddr*>(&local),sizeof(local))){closesocket(socketHandle);throw std::runtime_error("无法绑定上游地址");}
                // The first SYN to the alternate port must already have an
                // exact route. Publishing the ephemeral source only after
                // connect() returned meant that the SYN escaped untranslated
                // and Windows rejected the connection before it could reach
                // a local accelerator such as Xunyou.
                sockaddr_in bound{};int boundSize=sizeof(bound);
                if(getsockname(socketHandle,reinterpret_cast<sockaddr*>(&bound),&boundSize)){
                    closesocket(socketHandle);throw std::runtime_error("无法读取上游端点");
                }
                {std::lock_guard lock(s->mutex);s->route.upstream=endpoint(bound);}
            }
            u_long nonblock=1;ioctlsocket(socketHandle,FIONBIO,&nonblock);
            if(connect(socketHandle,reinterpret_cast<sockaddr*>(&dest),sizeof(dest)) && WSAGetLastError()!=WSAEWOULDBLOCK){auto error=WSAGetLastError();closesocket(socketHandle);throw std::runtime_error("上游连接失败 "+std::to_string(error));}
            fd_set writable;FD_ZERO(&writable);FD_SET(socketHandle,&writable);timeval timeout{8,0};if(select(0,nullptr,&writable,nullptr,&timeout)<=0){closesocket(socketHandle);throw std::runtime_error("上游连接超时");}
            int error=0,size=sizeof(error);getsockopt(socketHandle,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size);if(error){closesocket(socketHandle);throw std::runtime_error("上游连接错误 "+std::to_string(error));}
            nonblock=0;ioctlsocket(socketHandle,FIONBIO,&nonblock);return socketHandle;
        };
        const auto proxyPort=s->https?uint16_t(18080):uint16_t(0);
        if(proxyPort){
            try{
                sockaddr_in proxy{};proxy.sin_family=AF_INET;proxy.sin_addr.s_addr=htonl(INADDR_LOOPBACK);proxy.sin_port=htons(uint16_t(proxyPort));
                // A socket bound to the physical adapter cannot reliably connect to
                // loopback. Let Windows select a loopback source for the local proxy.
                remote=openRemote(proxy,false);
                char host[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,s->route.remote.address.data()+12,host,sizeof(host));
                const auto authority=std::string(host)+":"+std::to_string(s->route.remote.port);
                const auto request="CONNECT "+authority+" HTTP/1.1\r\nHost: "+authority+"\r\nProxy-Connection: keep-alive\r\n\r\n";
                sendAll(remote,std::span(reinterpret_cast<const uint8_t*>(request.data()),request.size()));
                std::string response;std::array<char,1024> header{};
                while(response.find("\r\n\r\n")==std::string::npos && response.size()<16384){const auto n=recv(remote,header.data(),int(header.size()),0);if(n<=0)throw std::runtime_error("本地 HTTPS 代理未响应");response.append(header.data(),size_t(n));}
                const auto lineEnd=response.find("\r\n");const auto first=response.substr(0,lineEnd);
                if(first.find(" 200 ")==std::string::npos)throw std::runtime_error("本地 HTTPS 代理拒绝 CONNECT: "+first);
                diagnostics().write("chat_https_proxy","connection="+std::to_string(s->view.id)+" port="+std::to_string(proxyPort)+" target="+authority+" connected=1");
            }catch(const std::exception& e){
                if(remote!=INVALID_SOCKET){closesocket(remote);remote=INVALID_SOCKET;}
                diagnostics().write("chat_https_proxy_fallback","connection="+std::to_string(s->view.id)+" reason="+e.what());
            }
        }
        if(remote==INVALID_SOCKET){auto dest=address(s->route.remote);dest.sin_port=htons(s->route.alternatePort);remote=openRemote(dest);}
        {std::lock_guard lock(s->mutex);s->remote=remote;sockaddr_in local{};int size=sizeof(local);getsockname(remote,reinterpret_cast<sockaddr*>(&local),&size);s->route.upstream=endpoint(local);}
        message("连接已接管，等待自动握手");
        std::array<uint8_t,65536> buffer{};
        while(!clientEof || !remoteEof){
            if(clientEof || remoteEof){
                if(!halfClosedAt)halfClosedAt=GetTickCount64();
                if(GetTickCount64()-halfClosedAt>=15000)throw std::runtime_error("半关闭连接等待超时，释放代理连接");
            }
            if(!forwarding_ || !running_)throw std::runtime_error("代理正在关闭");
            Bytes mailWire;
            {
                std::lock_guard lock(s->mutex);auto now=GetTickCount64();s->mail.tick(now);
                if(s->mail.command){
                    if(disabled || native || ours || clientEof || remoteEof)s->mail.cancelQueued();
                    else if(cipher.ready() && cipher.clientBoundary()){
                        mailWire=cipher.mail(*s->mail.command);s->mail.submitted(now);s->view.shifted=true;
                    }
                }
            }
            if(!mailWire.empty()){
                sendAll(remote,mailWire);observe();
                diagnostics().write("mail_submitted","connection="+std::to_string(s->view.id)+" bytes="+std::to_string(mailWire.size()));
            }
            Bytes guildWire;
            {
                std::lock_guard lock(s->mutex);auto now=GetTickCount64();
                if(s->guildCommand){
                    if(disabled || native || ours || clientEof || remoteEof || now-s->guildQueuedAt>5000){
                        s->guildCommand.reset();s->view.guildPending=false;s->view.guildStatus="军团查询未发送：连接忙碌或不可用";
                    }else if(cipher.ready() && cipher.clientBoundary()){
                        auto c=std::move(*s->guildCommand);s->guildCommand.reset();
                        guildWire=cipher.guild(c.search,c.order,c.name);s->guildSentAt=now;s->view.shifted=true;
                    }
                }
                if(s->view.guildPending && s->guildSentAt && now-s->guildSentAt>15000){s->view.guildPending=false;s->view.guildStatus="15 秒内未收到对应类型响应；结果未知，不自动重发";}
            }
            if(!guildWire.empty()){
                sendAll(remote,guildWire);observe();std::lock_guard lock(s->mutex);
                ++s->view.guildSent;s->view.guildStatus="已发送，等待军团响应";
                diagnostics().write("guild_submitted","connection="+std::to_string(s->view.id)+" expected_opcode="+std::to_string(s->guildExpected));
            }
            Bytes queryWire;
            {
                std::lock_guard lock(s->mutex);
                if(s->command && cipher.clientBoundary() && !clientEof && !remoteEof && !native && !disabled){
                    queryWire=cipher.query(s->command->first,s->command->second);s->command.reset();
                    // A failed/partial socket send is ambiguous; never retry application requests.
                    s->view.shifted=true;ours=true;ambiguous=false;waitingSince=GetTickCount64();
                }
                if(s->command && (disabled||native||clientEof||remoteEof||GetTickCount64()-s->queuedAt>5000)){if(s->receipt)s->receipt->fail("not_sent_connection_busy");s->command.reset();s->view.waiting=native;s->view.status="查询未发送：连接繁忙、关闭或未到帧边界";}
                if(!disabled && (ours||native) && GetTickCount64()-waitingSince>15000){if(ours && s->receipt)s->receipt->fail("game_response_timeout");s->view.status="资料响应超时；为避免错配，本连接不再接受新查询";s->view.ready=false;disabled=true;diagnostics().write("query_timeout","connection="+std::to_string(s->view.id)+" own="+std::to_string(ours)+" elapsed_ms="+std::to_string(GetTickCount64()-waitingSince));}
            }
            if(!queryWire.empty()){
                sendAll(remote,queryWire);observe();std::lock_guard lock(s->mutex);if(s->receipt)s->receipt->sent();++s->view.sent;s->lastSent=waitingSince=GetTickCount64();s->view.status="已提交到服务器连接，等待资料响应";
                diagnostics().write("query_submitted","connection="+std::to_string(s->view.id));
            }
            {
                std::lock_guard lock(s->mutex);
                const auto now=GetTickCount64();
                auto cancel=[&](const char* reason){
                    s->jumpCommand=s->view.jumpPending=s->view.jumpRepeat=s->repeatArm=false;s->view.jumpNextInMs=0;jumpPlan.clear();
                    s->view.jumpStatus=reason;
                    diagnostics().write("jump_cancelled","connection="+std::to_string(s->view.id)+" sent_frames="+std::to_string(jumpIndex)+" reason="+reason);
                };
                if(s->repeatArm){
                    if(!jump.state(now).ready || disabled || clientEof || remoteEof)cancel("重复跳跃未启动：停止位置失效或连接状态变化");
                    else {s->repeatArm=false;s->repeatRevision=jump.revision();s->nextJumpAt=std::max(now,s->lastJumpSent?s->lastJumpSent+5000:now);}
                }
                if(s->view.jumpRepeat && (disabled || clientEof || remoteEof || jump.revision()!=s->repeatRevision))
                    cancel("重复跳跃已停止：游戏移动、切图、服务器校正或连接状态变化");
                if(s->view.jumpRepeat && jumpPlan.empty() && !s->jumpCommand && now>=s->nextJumpAt && !native && !ours && !s->command){
                    s->jumpCommand=s->view.jumpPending=true;s->jumpQueuedAt=now;
                }
                if(!jumpPlan.empty() && (disabled || native || ours || clientEof || remoteEof || jump.revision()!=jumpRevision))
                    cancel("剩余跳跃消息已取消：游戏动作、服务器校正或连接状态发生变化");
                if(s->jumpCommand){
                    if(disabled || native || ours || clientEof || remoteEof || now-s->jumpQueuedAt>500 || !jump.state(now,s->view.jumpRepeat).ready)
                        cancel("跳跃未发送：位置过期、状态已变化或连接忙");
                    else if(cipher.ready() && cipher.clientBoundary()){
                        try{
                            jumpPlan=jump.prepareSequence(now,s->view.jumpRepeat);jumpIndex=0;jumpStartedAt=now;jumpRevision=jump.revision();
                            s->jumpCommand=false;
                        }catch(const std::exception&){cancel("跳跃未发送：当前轨迹或时间不可用");}
                    }
                }
                if(!jumpPlan.empty() && now-jumpStartedAt>jumpPlan[jumpIndex].delayMs+250)
                    cancel("剩余跳跃消息已取消：发送时间偏差超过 250 毫秒，不补发");
            }
            if(!jumpPlan.empty() && cipher.clientBoundary() && GetTickCount64()-jumpStartedAt>=jumpPlan[jumpIndex].delayMs){
                // The session thread serializes native and generated traffic. Every
                // step advances only the upstream cipher; never retry partial sends.
                auto wire=cipher.jumpMotion(jumpPlan[jumpIndex].frame);
                sendAll(remote,wire);
                const auto now=GetTickCount64();
                if(jumpIndex==0)jump.submitted(now);
                observe();
                std::lock_guard lock(s->mutex);s->view.shifted=true;
                if(jumpIndex==0){++s->view.jumpsSent;s->lastJumpSent=now;if(s->view.jumpRepeat)s->nextJumpAt=now+uint64_t(s->view.jumpIntervalSeconds)*1000;}
                ++jumpIndex;
                s->view.jumpStatus="正在发送完整跳跃："+std::to_string(jumpIndex)+" / "+std::to_string(jumpPlan.size());
                diagnostics().write("jump_sequence_frame","connection="+std::to_string(s->view.id)+" index="+std::to_string(jumpIndex)+" elapsed_ms="+std::to_string(now-jumpStartedAt));
                if(jumpIndex==jumpPlan.size()){
                    jump.completed(*decodeJumpMotion(jumpPlan.back().frame),now);
                    jumpPlan.clear();s->view.jumpPending=false;
                    s->view.jumpStatus="完整起跳、空中更新、下落和停止消息已提交；服务器结果待确认";
                }
            }
            fd_set read;FD_ZERO(&read);if(!clientEof)FD_SET(client,&read);if(!remoteEof)FD_SET(remote,&read);
            timeval tv{0,jumpPlan.empty()?100000L:5000L};int selected=select(0,&read,nullptr,nullptr,&tv);
            if(selected<0)throw std::runtime_error("连接读取失败");
            if(!clientEof && FD_ISSET(client,&read)){
                int n=recv(client,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0);
                if(n<0)throw std::runtime_error("游戏连接读取失败");
                if(!n){if(!opaque && !cipher.clientBoundary())throw std::runtime_error("游戏连接在发送帧中间关闭");clientEof=true;jump.ended(GetTickCount64(),"client_eof");flushJumpEvidence();shutdown(remote,SD_SEND);}
                else if(opaque){auto b=std::span(buffer).first(size_t(n));sendAll(remote,b);if(login)observeLogin(b,true);}
                else{auto wire=cipher.fromClient(std::span(buffer).first(size_t(n)));sendAll(remote,wire);observe();
                    if(cipher.takeNativeQuery()){native=true;waitingSince=GetTickCount64();if(ours){ambiguous=true;disabled=true;}std::lock_guard lock(s->mutex);s->view.waiting=true;}}
            }
            if(!remoteEof && FD_ISSET(remote,&read)){
                int n=recv(remote,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0);
                if(n<0)throw std::runtime_error("服务器连接读取失败");
                if(!n){if(!opaque && !cipher.serverBoundary())throw std::runtime_error("服务器连接在响应帧中间关闭");remoteEof=true;jump.ended(GetTickCount64(),"server_eof");flushJumpEvidence();shutdown(client,SD_SEND);}
                else if(opaque){auto b=std::span(buffer).first(size_t(n));sendAll(client,b);if(login)observeLogin(b,false);}
                else{auto wire=cipher.fromServer(std::span(buffer).first(size_t(n)));observe();
                    auto forwarded=(ours||native)?examine(examine,wire,0):std::move(wire);sendAll(client,forwarded);
                }
            }
            jump.tick(GetTickCount64());flushJumpEvidence();
            {std::lock_guard lock(s->mutex);s->view.ready=!opaque && cipher.ready() && jump.worldEntered() && !disabled && !clientEof && !remoteEof;
                s->view.jumpState=jump.state(GetTickCount64(),s->view.jumpRepeat);
                const auto nextNow=GetTickCount64();s->view.jumpNextInMs=s->view.jumpRepeat && s->nextJumpAt>nextNow?s->nextJumpAt-nextNow:0;
                if(!s->view.ready || native || ours || s->command || s->jumpCommand || s->view.jumpPending)s->view.jumpState.ready=false;
                if(s->lastJumpSent && GetTickCount64()-s->lastJumpSent<5000){s->view.jumpState.ready=false;s->view.jumpState.status="两次实验发送至少间隔 5 秒";}
                const std::string currentStatus=s->https?"聊天 HTTPS/WSS：经 18080 选择性接管":login?"登录连接：自动转发与记录（不支持玩家查询）":cipher.ready() && !jump.worldEntered()?"握手已完成，等待自身角色进入场景":cipher.status();
                if(lastCipherStatus!=currentStatus){lastCipherStatus=currentStatus;s->view.status=lastCipherStatus;diagnostics().write("query_handshake",lastCipherStatus);}}
        }
        message("连接已结束");
    }catch(const std::exception& e){message(std::string("连接结束：")+e.what());diagnostics().write("query_connection_error",e.what());jump.ended(GetTickCount64(),"transport_error");flushJumpEvidence();shutdown(client,SD_BOTH);if(remote!=INVALID_SOCKET)shutdown(remote,SD_BOTH);}
    {std::lock_guard lock(s->mutex);closesocket(client);if(remote!=INVALID_SOCKET)closesocket(remote);s->client=s->remote=INVALID_SOCKET;
        s->view.open=s->view.ready=s->view.waiting=false;s->command.reset();
        s->mail.close();
        s->guildCommand.reset();if(s->view.guildPending)s->view.guildStatus="连接结束，军团查询未完成；不自动重试";s->view.guildPending=false;
        if(s->receipt)s->receipt->fail("game_connection_closed");
        if(s->view.jumpPending)s->view.jumpStatus="连接结束，未完成的跳跃消息不再发送，不自动重试";
        s->jumpCommand=s->view.jumpPending=s->view.jumpState.ready=s->view.jumpRepeat=s->repeatArm=false;s->view.jumpNextInMs=0;}
    if(login)for(int i=0;i<2;++i)if(!loginPending[i].empty())publish(*s,{i==0,false,false,loginPending[i],loginPending[i]});
    publish(*s,{true,false,false,{},{}},0x11);publish(*s,{false,false,false,{},{}},0x11);
    --active_;
    {std::lock_guard lock(s->mutex);s->closedAt=GetTickCount64();}
}
}
