#include "chat_bridge.hpp"
#include "diagnostics.hpp"
#include "query_proxy.hpp"
#include <nlohmann/json.hpp>
#include <set>
#include <fstream>
#include <vector>

namespace aion {
namespace {
std::filesystem::path executableDirectory(){
    std::vector<wchar_t> path(32768);const auto n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!n || n>=path.size())return {};return std::filesystem::path(std::wstring(path.data(),n)).parent_path();
}
std::filesystem::path configuredBridge(){
    wchar_t configured[32768]{};const auto n=GetEnvironmentVariableW(L"AION2_CHAT_BRIDGE",configured,DWORD(std::size(configured)));
    if(n && n<std::size(configured))return configured;
    return executableDirectory()/L"chat_bridge"/L"Aion2ChatBridge.exe";
}
std::string utf8(const std::wstring& value){
    if(value.empty())return {};const auto n=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(size_t(n),0);WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),result.data(),n,nullptr,nullptr);return result;
}
struct EnvironmentRestore {
    std::wstring name,value;bool existed{};
    EnvironmentRestore(const wchar_t* key,const wchar_t* replacement):name(key){
        wchar_t buffer[32768]{};const auto n=GetEnvironmentVariableW(key,buffer,DWORD(std::size(buffer)));
        existed=n && n<std::size(buffer);if(existed)value.assign(buffer,n);SetEnvironmentVariableW(key,replacement);
    }
    ~EnvironmentRestore(){SetEnvironmentVariableW(name.c_str(),existed?value.c_str():nullptr);}
};
}

ChatBridge::ChatBridge():executable_(configuredBridge()),log_(executableDirectory()/L"chat-bridge.log"){}
ChatBridge::~ChatBridge(){stop();}

bool ChatBridge::start(){
    refresh();if(process_!=INVALID_HANDLE_VALUE)return true;
    if(!std::filesystem::exists(executable_)){message_="聊天组件未安装："+utf8(executable_.wstring());return false;}
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE output=CreateFileW(log_.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output==INVALID_HANDLE_VALUE){message_="无法创建聊天组件日志："+std::to_string(GetLastError());return false;}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    startup.hStdOutput=output;startup.hStdError=output;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};auto command=L"\""+executable_.wstring()+L"\"";
    EnvironmentRestore managed(L"AION2_MANAGED_BY_PIPE",L"1"),tool(L"AION2_MITM_TOOL",L"mitmdump"),browser(L"AION2_WEB_OPEN_BROWSER",L"0"),certificate(L"AION2_AUTO_INSTALL_CERT",L"1");
    const auto identityFile=(executableDirectory()/L"game-server-status.json").wstring();
    EnvironmentRestore gameIdentity(L"AION2_GAME_SERVER_STATUS",identityFile.c_str());
    std::unique_ptr<EnvironmentRestore> mitmMode,allowHosts;
    // Aion2Pipe selectively forwards only AION2.exe TCP/443 to this listener.
    // The bridge never detects or redirects accelerator processes.
    mitmMode=std::make_unique<EnvironmentRestore>(L"AION2_MITM_MODE",L"regular@127.0.0.1:18080");
    // CONNECT uses the destination IP, so filtering happens in the addon after
    // TLS SNI/HTTP headers are available rather than rejecting the tunnel early.
    allowHosts=std::make_unique<EnvironmentRestore>(L"AION2_ALLOW_HOSTS",L"");
    const auto directory=executable_.parent_path().wstring();
    const bool created=CreateProcessW(executable_.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,directory.c_str(),&startup,&info)!=FALSE;
    CloseHandle(output);
    if(!created){message_="聊天组件启动失败："+std::to_string(GetLastError());diagnostics().write("chat_bridge_error",message_);return false;}
    process_=info.hProcess;processId_=info.dwProcessId;exitCode_=STILL_ACTIVE;
    job_=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job_ || !SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) ||
       !AssignProcessToJobObject(job_,process_) || ResumeThread(info.hThread)==DWORD(-1)){
        const auto error=GetLastError();TerminateProcess(process_,1);WaitForSingleObject(process_,5000);
        CloseHandle(info.hThread);CloseHandle(process_);process_=INVALID_HANDLE_VALUE;processId_=0;
        if(job_){CloseHandle(job_);job_=nullptr;}
        message_="无法建立聊天组件退出联动："+std::to_string(error);diagnostics().write("chat_bridge_error",message_);return false;
    }
    CloseHandle(info.hThread);
    message_="聊天接管组件正在运行";diagnostics().write("chat_bridge_start","pid="+std::to_string(processId_)+" executable="+utf8(executable_.wstring()));return true;
}

void ChatBridge::stop(){
    refresh();if(process_==INVALID_HANDLE_VALUE)return;
    diagnostics().write("chat_bridge_stop","pid="+std::to_string(processId_));
    if(job_){CloseHandle(job_);job_=nullptr;}else TerminateProcess(process_,0);
    WaitForSingleObject(process_,5000);CloseHandle(process_);process_=INVALID_HANDLE_VALUE;processId_=0;message_="聊天接管组件已停止";
}

void ChatBridge::refresh(){
    if(process_==INVALID_HANDLE_VALUE)return;DWORD code=STILL_ACTIVE;
    if(GetExitCodeProcess(process_,&code) && code!=STILL_ACTIVE){exitCode_=code;CloseHandle(process_);process_=INVALID_HANDLE_VALUE;
        if(job_){CloseHandle(job_);job_=nullptr;}processId_=0;message_="聊天接管组件已退出，代码 "+std::to_string(code);diagnostics().write("chat_bridge_exit",message_);}
}

ChatBridgeStatus ChatBridge::status(){
    refresh();ChatBridgeStatus value;value.installed=std::filesystem::exists(executable_);value.running=process_!=INVALID_HANDLE_VALUE;value.processId=processId_;value.exitCode=exitCode_;value.message=message_;
    std::ifstream input(log_,std::ios::binary);if(input){input.seekg(0,std::ios::end);const auto length=input.tellg();const auto keep=std::streamoff(8192);const auto offset=length>std::streampos(keep)?length-keep:std::streampos(0);input.seekg(offset);value.logTail.assign(std::istreambuf_iterator<char>(input),{});}
    return value;
}
void ChatBridge::syncGameServer(const QueryProxy& proxy){
    const auto tick=GetTickCount64();if(identityTick_ && tick-identityTick_<2000)return;identityTick_=tick;
    std::set<uint32_t> servers;
    for(const auto& connection:proxy.connections())if(connection.open && connection.ready && connection.serverId)servers.insert(connection.serverId);
    // Multiple game servers are ambiguous; never attribute another connection's
    // character to an arbitrary first server. An empty value also clears stale UI.
    using Json=nlohmann::json;
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const Json data={{"serverId",servers.size()==1?Json(std::to_string(*servers.begin())):Json(nullptr)},{"updatedAt",now}};
    const auto path=executableDirectory()/L"game-server-status.json";auto temporary=path;temporary+=L".tmp";
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);if(!output)return;
    output<<data.dump();output.close();if(!output)return;
    MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
}
