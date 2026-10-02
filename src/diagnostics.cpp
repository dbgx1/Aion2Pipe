#include "diagnostics.hpp"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <map>
#include <regex>
#include <vector>
namespace aion {
namespace {
void pruneHistory(const std::filesystem::path& directory) noexcept {
    try {
        // Only generated, regular log segments from exited processes are eligible.
        const std::wregex pattern(L"^(Aion2Pipe-[0-9]{8}-[0-9]{6}-[0-9]{3}-([0-9]+)-[0-9]+\\.log)(\\.[12])?$");
        std::map<std::wstring,std::vector<std::filesystem::path>,std::greater<>> sessions;
        std::map<DWORD,bool> exited;
        for(const auto& entry:std::filesystem::directory_iterator(directory)){
            if(entry.symlink_status().type()!=std::filesystem::file_type::regular)continue;
            const auto name=entry.path().filename().wstring();std::wsmatch match;
            if(!std::regex_match(name,match,pattern))continue;
            const auto number=std::stoull(match[2].str());if(!number || number>MAXDWORD)continue;
            const auto pid=DWORD(number);
            if(!exited.contains(pid)){
                HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
                if(process){exited[pid]=WaitForSingleObject(process,0)==WAIT_OBJECT_0;CloseHandle(process);}
                else exited[pid]=GetLastError()==ERROR_INVALID_PARAMETER; // Access denied is not proof of exit.
            }
            if(exited[pid])sessions[match[1].str()].push_back(entry.path());
        }
        size_t retained=0;
        for(const auto& [name,files]:sessions){
            if(++retained<=20)continue;
            for(const auto& file:files){std::error_code ec;std::filesystem::remove(file,ec);}
        }
    }catch(...){/* Cleanup must never prevent diagnostics startup. */}
}
std::string timestamp(bool filename=false){SYSTEMTIME s{};GetSystemTime(&s);char text[64];
    if(filename)snprintf(text,sizeof(text),"%04u%02u%02u-%02u%02u%02u-%03u",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond,s.wMilliseconds);
    else snprintf(text,sizeof(text),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond,s.wMilliseconds);return text;}
}
std::string utf8Path(const std::filesystem::path& p){auto s=p.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
Diagnostics& diagnostics(){static Diagnostics instance;return instance;}
bool Diagnostics::initialize(const std::filesystem::path& primary,const std::filesystem::path& fallback,size_t limit){
    std::lock_guard lock(mutex_);stream_.close();stream_.clear();path_.clear();error_.clear();bytes_=0;limit_=std::max<size_t>(limit,256);start_=std::chrono::steady_clock::now();
    retryAt_={};static std::atomic<unsigned> serial{};
    auto name="Aion2Pipe-"+timestamp(true)+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(++serial)+".log";
    for(const auto& dir:{primary,fallback}){if(dir.empty())continue;std::error_code ec;std::filesystem::create_directories(dir,ec);if(ec)continue;
        auto file=dir/name;stream_.open(file,std::ios::binary|std::ios::app);if(stream_){path_=file;pruneHistory(dir);return true;}stream_.clear();}
    error_="日志目录不可写，未能记录诊断信息。";return false;
}
std::filesystem::path Diagnostics::part(unsigned i) const {auto p=path_;if(i)p+=L"."+std::to_wstring(i);return p;}
void Diagnostics::write(std::string_view event,std::string_view details) noexcept {
    try {std::lock_guard lock(mutex_);if(path_.empty())return;
        if(!stream_.is_open() || !stream_){
            const auto now=std::chrono::steady_clock::now();if(now<retryAt_)return;
            retryAt_=now+std::chrono::seconds(1);
            stream_.close();stream_.clear();
            stream_.open(path_,std::ios::binary|std::ios::app);
            if(!stream_)return;
            bytes_=size_t(std::filesystem::file_size(path_));
        }
        auto line=nlohmann::json{{"time_utc",timestamp()},{"uptime_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start_).count()},
            {"event",std::string(event.substr(0,128))},{"details",std::string(details.substr(0,16384))}}.dump(-1,' ',false,nlohmann::json::error_handler_t::replace)+"\n";
        if(bytes_ && bytes_+line.size()>limit_){stream_.close();std::error_code ec;
            std::filesystem::remove(part(2),ec);if(ec)throw std::runtime_error("日志轮换失败");
            if(std::filesystem::exists(part(1)))std::filesystem::rename(part(1),part(2));std::filesystem::rename(part(0),part(1));
            stream_.open(path_,std::ios::binary|std::ios::trunc);bytes_=0;
        }
        stream_.write(line.data(),std::streamsize(line.size()));stream_.flush();if(!stream_){error_="诊断日志写入失败，请检查磁盘空间。";retryAt_=std::chrono::steady_clock::now()+std::chrono::seconds(1);}else{bytes_+=line.size();error_.clear();}
    }catch(...){try{std::lock_guard lock(mutex_);stream_.close();stream_.clear();retryAt_=std::chrono::steady_clock::now()+std::chrono::seconds(1);error_="诊断日志写入或轮换失败，将自动重试。";}catch(...) {}}
}
std::filesystem::path Diagnostics::path() const {std::lock_guard lock(mutex_);return path_;}
std::string Diagnostics::error() const {std::lock_guard lock(mutex_);return error_;}
bool Diagnostics::exportTo(const std::filesystem::path& destination,std::string& error){
    std::lock_guard lock(mutex_);
    try {if(path_.empty())throw std::runtime_error("没有可导出的日志");stream_.flush();
        for(unsigned i=0;i<3;++i){auto source=part(i);std::error_code ec;
            if(std::filesystem::absolute(destination).lexically_normal()==std::filesystem::absolute(source).lexically_normal() ||
                (std::filesystem::exists(destination) && std::filesystem::exists(source) && std::filesystem::equivalent(destination,source,ec)))
                throw std::runtime_error("请选择另一个文件名，不能覆盖正在使用的日志");}
        std::ofstream out(destination,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("无法创建导出日志");
        for(int i=2;i>=0;--i){auto p=part(unsigned(i));if(!std::filesystem::exists(p))continue;std::ifstream in(p,std::ios::binary);if(!in)throw std::runtime_error("无法读取日志分段");
            char buffer[8192];while(in.read(buffer,sizeof(buffer)) || in.gcount()){out.write(buffer,in.gcount());}if(in.bad())throw std::runtime_error("读取日志失败");}
        out.close();if(!out)throw std::runtime_error("导出日志写入失败");return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
void initializeDiagnostics(){
    wchar_t exe[32768]{},local[32768]{};GetModuleFileNameW(nullptr,exe,32768);GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    auto folder=std::filesystem::path(exe).parent_path();auto fallback=*local?std::filesystem::path(local)/L"Aion2Pipe"/L"logs":std::filesystem::path{};
    auto& log=diagnostics();log.initialize(folder/L"logs",fallback);
    TOKEN_ELEVATION elevation{};DWORD size=0;HANDLE token=nullptr;if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size);CloseHandle(token);}
    OSVERSIONINFOW os{};os.dwOSVersionInfoSize=sizeof(os);auto ntdll=GetModuleHandleW(L"ntdll.dll");
    using Version=LONG(WINAPI*)(OSVERSIONINFOW*);auto version=reinterpret_cast<Version>(GetProcAddress(ntdll,"RtlGetVersion"));if(version)version(&os);
    std::ostringstream info;info<<"build="<<__DATE__<<" "<<__TIME__<<" source_id="<<AION_BUILD_ID<<" pid="<<GetCurrentProcessId()<<" elevated="<<elevation.TokenIsElevated<<" windows="<<os.dwMajorVersion<<'.'<<os.dwMinorVersion<<'.'<<os.dwBuildNumber
        <<" arch=x64 world_port=13328 login_port=13700 schema=2026-09-24 log_format=1";
    log.write("startup",info.str());
    for(auto name:{L"Aion2Pipe.exe",L"WinDivert.dll",L"WinDivert64.sys"}){auto p=folder/name;std::error_code ec;bool exists=std::filesystem::exists(p,ec);auto bytes=exists?std::filesystem::file_size(p,ec):0;
        log.write("runtime_file",utf8Path(name)+" exists="+std::to_string(exists)+" bytes="+std::to_string(ec?0:bytes));}
    log.write("privacy","Metadata only: no packet payloads, character names, chat, session keys, or remote IP addresses. Three 4 MiB segments per run; export combines them oldest first. Startup retains newest 20 exited-process log sessions; active/unknown processes and other files are preserved.");
}
}
