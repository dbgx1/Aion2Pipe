#include "capture.hpp"
#include "diagnostics.hpp"
#include "owner_selection.hpp"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>

namespace aion {
static std::string winError(DWORD code) {
    char* msg=nullptr; FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,reinterpret_cast<char*>(&msg),0,nullptr);
    std::string text=msg?msg:"Unknown error"; if(msg) LocalFree(msg);
    if(code==5) text="Run Aion2Pipe as administrator to capture. Offline analysis needs no elevation.";
    if(code==2) text="WinDivert64.sys is missing beside the application.";
    if(code==577 || code==1275) text="Windows blocked the WinDivert driver. Check driver signature / security software policy.";
    return std::to_string(code)+": "+text;
}
static Endpoint v4(DWORD address,DWORD port) {
    Endpoint e; e.address[10]=e.address[11]=255; std::memcpy(e.address.data()+12,&address,4); e.port=ntohs(static_cast<u_short>(port)); return e;
}
static Endpoint v6(const UCHAR* address,DWORD port) {
    Endpoint e; std::copy_n(address,16,e.address.begin()); e.port=ntohs(static_cast<u_short>(port)); return e;
}
static Endpoint fromFlow(const UINT32* address,UINT16 port) {
    UINT32 network[4]; WinDivertHelperHtonIPv6Address(address,network);
    Endpoint e; std::memcpy(e.address.data(),network,16); e.port=port; return e;
}
Capture::~Capture(){stop();}
void Capture::setStatus(std::string s){diagnostics().write("capture_status",s);std::lock_guard lock(mutex_); status_=std::move(s);}
std::string Capture::status() const {std::lock_guard lock(mutex_); return status_;}
CaptureStats Capture::stats() const {std::lock_guard lock(mutex_); return stats_;}
bool Capture::isTarget(uint32_t pid) const {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); if(!process) return false;
    wchar_t path[32768]; DWORD length=32768;
    const BOOL ok=QueryFullProcessImageNameW(process,0,path,&length); CloseHandle(process);
    if(!ok) return false;
    const wchar_t* basename=wcsrchr(path,L'\\'); basename=basename?basename+1:path;
    return _wcsicmp(basename,executable_.c_str())==0;
}
bool Capture::start(const std::wstring& exe) {
    stop();
    if(exe.empty() || exe.find_first_of(L"/\\")!=std::wstring::npos) {setStatus("Enter a process filename, for example Aion2.exe"); return false;}
    executable_=exe;
    lastTargetConnections_.clear();
    {std::lock_guard lock(mutex_); stats_={}; owners_.clear(); pending_.clear(); ready_.clear(); pendingBytes_=readyBytes_=0;}
    LARGE_INTEGER now,freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq); baseQpc_=now.QuadPart; frequency_=freq.QuadPart;
    baseUs_=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    flow_=WinDivertOpen("tcp or udp",WINDIVERT_LAYER_FLOW,0,WINDIVERT_FLAG_SNIFF|WINDIVERT_FLAG_RECV_ONLY);
    if(flow_==INVALID_HANDLE_VALUE) {setStatus("FLOW: "+winError(GetLastError())); return false;}
    network_=WinDivertOpen("(tcp or udp) and !impostor",WINDIVERT_LAYER_NETWORK,0,WINDIVERT_FLAG_SNIFF|WINDIVERT_FLAG_RECV_ONLY);
    if(network_==INVALID_HANDLE_VALUE) {const auto error=GetLastError(); WinDivertClose(flow_); flow_=INVALID_HANDLE_VALUE; setStatus("NETWORK: "+winError(error)); return false;}
    WinDivertSetParam(network_,WINDIVERT_PARAM_QUEUE_LENGTH,8192);
    WinDivertSetParam(network_,WINDIVERT_PARAM_QUEUE_SIZE,16*1024*1024);
    WinDivertSetParam(network_,WINDIVERT_PARAM_QUEUE_TIME,1000);
    running_=true; receiverFinished_=false; flowFinished_=false; setStatus("Listening / 正在监听（等待目标进程流量）");
    try {
        receiver_=std::thread(&Capture::receiveLoop,this);
        flows_=std::thread(&Capture::flowLoop,this);
        resolver_=std::thread(&Capture::resolveLoop,this);
    } catch(const std::exception& e) {if(!receiver_.joinable()) receiverFinished_=true; if(!flows_.joinable()) flowFinished_=true; stop(); setStatus(e.what()); return false;}
    return true;
}
void Capture::stop() {
    running_=false;
    if(network_!=INVALID_HANDLE_VALUE) WinDivertShutdown(network_,WINDIVERT_SHUTDOWN_BOTH);
    if(flow_!=INVALID_HANDLE_VALUE) WinDivertShutdown(flow_,WINDIVERT_SHUTDOWN_BOTH);
    if(receiver_.joinable()) receiver_.join();
    if(flows_.joinable()) flows_.join();
    if(resolver_.joinable()) resolver_.join();
    if(network_!=INVALID_HANDLE_VALUE) WinDivertClose(network_);
    if(flow_!=INVALID_HANDLE_VALUE) WinDivertClose(flow_);
    network_=flow_=INVALID_HANDLE_VALUE;
    {std::lock_guard lock(mutex_); if(status_.starts_with("Listening")) status_="Stopped / 已停止";}
}
void Capture::receiveLoop() {
    Bytes buffer(65535); WINDIVERT_ADDRESS address{};
    while(running_) {
        UINT length=0;
        if(!WinDivertRecv(network_,buffer.data(),UINT(buffer.size()),&length,&address)) {
            const auto e=GetLastError();
            if(running_) {setStatus("Capture stopped: "+winError(e)); running_=false;}
            break;
        }
        Pending item; std::string err;
        const bool valid=parsePacket(std::span(buffer).first(length),item.packet,err);
        std::lock_guard lock(mutex_); ++stats_.received;
        if(!valid) {++stats_.unsupported; continue;}
        item.qpc=address.Timestamp; item.queuedMs=GetTickCount64(); item.packet.outbound=address.Outbound!=0;
        const auto delta=address.Timestamp-baseQpc_;
        item.packet.timeUs=uint64_t(int64_t(baseUs_)+(delta/frequency_)*1000000+(delta%frequency_)*1000000/frequency_);
        size_t size=item.packet.raw.size()+item.packet.payload.size();
        if(pending_.size()>=8192 || pendingBytes_+size>32*1024*1024) {++stats_.queueDropped; continue;}
        pendingBytes_+=size; pending_.push_back(std::move(item));
    }
    receiverFinished_=true;
}
void Capture::flowLoop() {
    WINDIVERT_ADDRESS address{};
    while(running_) {
        if(!WinDivertRecv(flow_,nullptr,0,nullptr,&address)) {
            if(running_) {setStatus("Flow tracking stopped: "+winError(GetLastError())); running_=false;}
            break;
        }
        const auto& d=address.Flow;
        std::lock_guard lock(mutex_);
        if(address.Event==WINDIVERT_EVENT_FLOW_DELETED) {
            for(auto& f:owners_) if(f.endpoint==d.EndpointId && !f.end) f.end=address.Timestamp;
        } else if(address.Event==WINDIVERT_EVENT_FLOW_ESTABLISHED) {
            if(owners_.size()>=32768) {++stats_.queueDropped; continue;}
            Owner o{fromFlow(d.LocalAddr,d.LocalPort),fromFlow(d.RemoteAddr,d.RemotePort),d.Protocol,d.ProcessId,false};
            owners_.push_back({o,address.Timestamp,0,d.EndpointId});
        }
    }
    flowFinished_=true;
}
std::vector<Capture::Owner> Capture::snapshotOwners(size_t& targets) {
    std::vector<Owner> out; std::set<DWORD> targetPids;
    auto snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap!=INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
        if(Process32FirstW(snap,&entry)) do {if(_wcsicmp(entry.szExeFile,executable_.c_str())==0) targetPids.insert(entry.th32ProcessID);} while(Process32NextW(snap,&entry));
        CloseHandle(snap);
    }
    targets=targetPids.size();
    // Include other owners too: a shared UDP port must not be blindly attributed to the target.
    auto table=[&](bool tcp,ULONG family) {
        ULONG size=0;
        auto get=[&](void* data){return tcp?GetExtendedTcpTable(data,&size,FALSE,family,TCP_TABLE_OWNER_PID_ALL,0):GetExtendedUdpTable(data,&size,FALSE,family,UDP_TABLE_OWNER_PID,0);};
        if(get(nullptr)!=ERROR_INSUFFICIENT_BUFFER) return;
        Bytes buffer(size);
        DWORD result=get(buffer.data());
        if(result==ERROR_INSUFFICIENT_BUFFER) {buffer.resize(size); result=get(buffer.data());}
        if(result!=NO_ERROR) return;
        if(tcp && family==AF_INET) {
            auto t=reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
            for(DWORD i=0;i<t->dwNumEntries;++i) {auto& r=t->table[i]; if(r.dwState!=MIB_TCP_STATE_LISTEN) out.push_back({v4(r.dwLocalAddr,r.dwLocalPort),v4(r.dwRemoteAddr,r.dwRemotePort),6,r.dwOwningPid,false});}
        } else if(tcp) {
            auto t=reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
            for(DWORD i=0;i<t->dwNumEntries;++i) {auto& r=t->table[i]; if(r.dwState!=MIB_TCP_STATE_LISTEN) out.push_back({v6(r.ucLocalAddr,r.dwLocalPort),v6(r.ucRemoteAddr,r.dwRemotePort),6,r.dwOwningPid,false});}
        } else if(family==AF_INET) {
            auto t=reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buffer.data());
            for(DWORD i=0;i<t->dwNumEntries;++i) {auto& r=t->table[i]; out.push_back({v4(r.dwLocalAddr,r.dwLocalPort),{},17,r.dwOwningPid,true});}
        } else {
            auto t=reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(buffer.data());
            for(DWORD i=0;i<t->dwNumEntries;++i) {auto& r=t->table[i]; out.push_back({v6(r.ucLocalAddr,r.dwLocalPort),{},17,r.dwOwningPid,true});}
        }
    };
    table(true,AF_INET); table(true,AF_INET6); table(false,AF_INET); table(false,AF_INET6);
    std::set<std::string> descriptions;size_t targetConnections=0;
    for(const auto& o:out)if(targetPids.contains(o.pid)){
        ++targetConnections;
        if(descriptions.size()>=64)continue;
        const auto& a=o.remote.address;
        const bool loopback=(a[10]==255 && a[11]==255 && a[12]==127) || (std::all_of(a.begin(),a.begin()+15,[](uint8_t b){return b==0;}) && a[15]==1);
        descriptions.insert("pid="+std::to_string(o.pid)+",protocol="+std::to_string(o.protocol)+",local_port="+std::to_string(o.local.port)+",remote_port="+std::to_string(o.remote.port)+",remote_loopback="+std::to_string(loopback));
    }
    std::ostringstream details;details<<"target_processes="<<targets<<" target_connections="<<targetConnections<<" sampled="<<descriptions.size();
    for(const auto& d:descriptions)details<<" ["<<d<<"]";
    if(details.str()!=lastTargetConnections_){lastTargetConnections_=details.str();diagnostics().write("target_connections",lastTargetConnections_);}
    return out;
}
void Capture::resolveLoop() {
    std::vector<Owner> tables; uint64_t refreshed=0; uint64_t id=0;
    for(;;) {
        auto now=GetTickCount64();
        if(now-refreshed>=250) {
            size_t targets=0; tables=snapshotOwners(targets); refreshed=now;
            std::lock_guard lock(mutex_); stats_.processes=targets; stats_.connections=tables.size();
        }
        std::deque<Pending> work; std::vector<Flow> owners;
        {
            std::lock_guard lock(mutex_);
            while(!pending_.empty() && (!running_ || now-pending_.front().queuedMs>=300)) {
                pendingBytes_-=pending_.front().packet.raw.size()+pending_.front().packet.payload.size();
                work.push_back(std::move(pending_.front())); pending_.pop_front();
            }
            owners=owners_;
            LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
            std::erase_if(owners_,[&](const Flow& f){return f.end && qpc.QuadPart-f.end>frequency_*2;});
        }
        std::map<uint32_t,bool> validated;
        for(auto& item:work) {
            auto& p=item.packet;
            auto exact=[&](const Owner& o,bool reverse) {
                const auto& local=reverse?p.destination:p.source;
                const auto& remote=reverse?p.source:p.destination;
                return o.protocol==p.protocol && o.local==local && o.remote==remote;
            };
            uint32_t pid=0; bool outbound=p.outbound, ambiguous=false, foundFlow=false;
            auto choose=[&](uint32_t candidate,bool direction) {
                if(pid && pid!=candidate) ambiguous=true;
                if(!pid || candidate!=pid) {pid=candidate; outbound=direction;}
            };
            // Match both orientations: WinDivert labels all loopback packets outbound.
            for(const auto& f:owners) {
                if(item.qpc<f.begin || (f.end && item.qpc>f.end)) continue;
                for(bool reverse:{false,true}) if(exact(f.owner,reverse)) {
                    if(!validated.contains(f.owner.pid)) validated[f.owner.pid]=isTarget(f.owner.pid);
                    if(validated[f.owner.pid]) {choose(f.owner.pid,!reverse); foundFlow=true;}
                }
            }
            TcpOwnerSelection tcpOwners;
            if(!foundFlow) for(const auto& o:tables) {
                for(bool reverse:{false,true}) {
                    const auto& local=reverse?p.destination:p.source;
                    bool match=o.udpWildcard ? p.protocol==17 && o.local.port==local.port && (o.local.address==local.address || (o.local.wildcard() && o.local.address[10]==local.address[10])) : exact(o,reverse);
                    if(match){if(p.protocol==6)tcpOwners.add(o.pid,reverse);else choose(o.pid,!reverse);}
                }
            }
            if(!foundFlow && p.protocol==6)pid=tcpOwners.select([&](uint32_t candidate){
                if(!validated.contains(candidate))validated[candidate]=isTarget(candidate);
                return validated[candidate];
            },outbound);
            if(pid && !validated.contains(pid)) validated[pid]=isTarget(pid);
            std::lock_guard lock(mutex_);
            if(!pid || ambiguous || !validated[pid]) {++stats_.unmatched; continue;}
            p.pid=pid; p.attributed=true; p.outbound=outbound; p.id=++id;
            size_t bytes=p.raw.size()+p.payload.size();
            if(ready_.size()>=8192 || readyBytes_+bytes>32*1024*1024) {++stats_.queueDropped; continue;}
            readyBytes_+=bytes; ready_.push_back(std::move(p)); ++stats_.matched;
        }
        if(!running_ && receiverFinished_ && flowFinished_) {
            std::lock_guard lock(mutex_);
            if(pending_.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
std::vector<Packet> Capture::drain() {
    std::lock_guard lock(mutex_);
    std::vector<Packet> out; out.reserve(ready_.size());
    while(!ready_.empty()) {out.push_back(std::move(ready_.front())); ready_.pop_front();}
    readyBytes_=0; return out;
}
}
