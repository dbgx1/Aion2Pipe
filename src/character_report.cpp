#include "character_report.hpp"
#include "character_report_defaults.hpp"
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <cstring>
#include <type_traits>
namespace aion {
namespace {
int64_t millis(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::wstring wide(const std::string& v){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,v.data(),int(v.size()),nullptr,0);if(n<=0)throw std::runtime_error("地址编码无效");std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,v.data(),int(v.size()),out.data(),n);return out;}
struct Http {HINTERNET h{};~Http(){if(h)WinHttpCloseHandle(h);}operator HINTERNET()const{return h;}};
struct Url {std::wstring host,path;INTERNET_PORT port{};bool tls{};};
Url parseUrl(const std::string& input){
    auto text=wide(input);URL_COMPONENTS c{};c.dwStructSize=sizeof(c);c.dwHostNameLength=c.dwUrlPathLength=c.dwExtraInfoLength=c.dwUserNameLength=c.dwPasswordLength=DWORD(-1);
    if(!WinHttpCrackUrl(text.c_str(),DWORD(text.size()),0,&c)||c.dwUserNameLength||c.dwPasswordLength||c.dwExtraInfoLength)throw std::runtime_error("请输入不含账号、查询参数的控制台地址");
    Url u{std::wstring(c.lpszHostName,c.dwHostNameLength),std::wstring(c.lpszUrlPath,c.dwUrlPathLength),c.nPort,c.nScheme==INTERNET_SCHEME_HTTPS};
    if(c.nScheme!=INTERNET_SCHEME_HTTPS && (c.nScheme!=INTERNET_SCHEME_HTTP || (u.host!=L"localhost" && u.host!=L"127.0.0.1" && u.host!=L"[::1]")))throw std::runtime_error("控制台必须使用 HTTPS（本机测试除外）");
    if(u.path!=L"" && u.path!=L"/" && u.path!=L"/api/characters/upload")throw std::runtime_error("请输入控制台根地址，或 /api/characters/upload 地址");
    u.path=L"/api/characters/upload";return u;
}
std::string targetKey(const CharacterReportConfig& c){auto u=parseUrl(c.url);std::wstring origin=u.host;std::transform(origin.begin(),origin.end(),origin.begin(),::towlower);return reportHash(std::string(reinterpret_cast<const char*>(origin.data()),origin.size()*sizeof(wchar_t))+":"+std::to_string(u.port)+(u.tls?"/https/nearby-v1":"/http/nearby-v1"));}
std::string protect(const std::string& v){DATA_BLOB in{DWORD(v.size()),(BYTE*)v.data()},out{};if(!CryptProtectData(&in,L"Aion2Pipe character report",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))throw std::runtime_error("无法保存令牌");std::string result;constexpr char hex[]="0123456789abcdef";for(DWORD i=0;i<out.cbData;++i){result+=hex[out.pbData[i]>>4];result+=hex[out.pbData[i]&15];}LocalFree(out.pbData);return result;}
std::string unprotect(const std::string& v){if(v.size()%2)throw std::runtime_error("令牌文件损坏");std::vector<BYTE> b;for(size_t i=0;i<v.size();i+=2)b.push_back(BYTE(std::stoul(v.substr(i,2),nullptr,16)));DATA_BLOB in{DWORD(b.size()),b.data()},out{};if(!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))throw std::runtime_error("令牌属于其他 Windows 用户，请重新配置");std::string result((char*)out.pbData,out.cbData);SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);return result;}
}
ReportHttpResult postCharacterReport(const CharacterReportConfig& config,const std::string& body){
    auto u=parseUrl(config.url);Http session{WinHttpOpen(L"Aion2Pipe/character-report-v1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.h)throw std::runtime_error("无法初始化上报网络");WinHttpSetTimeouts(session,3000,3000,5000,5000);
    Http connection{WinHttpConnect(session,u.host.c_str(),u.port,0)};if(!connection.h)throw std::runtime_error("无法连接控制台");
    Http request{WinHttpOpenRequest(connection,L"POST",u.path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,u.tls?WINHTTP_FLAG_SECURE:0)};
    if(!request.h)throw std::runtime_error("无法创建上报请求");DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));
    if(config.token.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("令牌格式无效");
    auto headers=wide("Content-Type: application/json\r\nAuthorization: Bearer "+config.token+"\r\n");
    auto begin=GetTickCount64();
    if(!WinHttpSendRequest(request,headers.c_str(),DWORD(headers.size()),(void*)body.data(),DWORD(body.size()),DWORD(body.size()),0)||!WinHttpReceiveResponse(request,nullptr))throw std::runtime_error("上报网络失败，错误码 "+std::to_string(GetLastError()));
    ReportHttpResult result;DWORD size=sizeof(result.status);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&result.status,&size,nullptr))throw std::runtime_error("无法读取 HTTP 状态");
    wchar_t after[128]{};size=sizeof(after);if(WinHttpQueryHeaders(request,WINHTTP_QUERY_RETRY_AFTER,WINHTTP_HEADER_NAME_BY_INDEX,after,&size,nullptr)){
        wchar_t* end{};auto seconds=wcstoll(after,&end,10);if(end!=after && *end==0 && seconds>=0)result.retryAfterMs=std::min<int64_t>(seconds,86400)*1000;
        else {SYSTEMTIME st{};FILETIME ft{};if(WinHttpTimeToSystemTime(after,&st)&&SystemTimeToFileTime(&st,&ft)){ULARGE_INTEGER i;i.LowPart=ft.dwLowDateTime;i.HighPart=ft.dwHighDateTime;result.retryAfterMs=std::clamp<int64_t>(int64_t(i.QuadPart/10000)-11644473600000LL-millis(),0,86400000);}}
    }
    for(;;){if(GetTickCount64()-begin>15000)throw std::runtime_error("上报响应超时");char buffer[8192];DWORD read{};if(!WinHttpReadData(request,buffer,sizeof(buffer),&read))throw std::runtime_error("读取上报响应失败");if(!read)break;result.body.append(buffer,read);if(result.body.size()>512000)throw std::runtime_error("上报响应过大");}return result;
}
struct CharacterReporter::Impl {
    std::filesystem::path dir;mutable std::mutex mutex;std::condition_variable cv;bool stop{},force{},retry{},fatal{},persisting{};
    CharacterReportConfig config{AION_REPORT_DEFAULT_URL,AION_REPORT_DEFAULT_TOKEN};CharacterReportStatus status;uint64_t generation{};
    std::map<std::string,ReportJson> inbox;std::thread worker;
    explicit Impl(std::filesystem::path d):dir(std::move(d)){
        try{std::ifstream f(dir/L"report-config.json");if(f){auto j=ReportJson::parse(f);auto savedUrl=j.value("url","");if(!savedUrl.empty())config.url=savedUrl;auto encrypted=j.value("protectedToken","");if(!encrypted.empty())config.token=unprotect(encrypted);}}catch(const std::exception& e){status.message=e.what();}
        worker=std::thread([this]{run();});
    }
    ~Impl(){{std::lock_guard lock(mutex);stop=true;status.enabled=false;}cv.notify_all();if(worker.joinable())worker.join();}
    void storageRecovered(){std::lock_guard lock(mutex);if(status.storageBlocked){status.storageBlocked=false;status.message="本地缓存读写已恢复";}}
    template<class Operation> auto storage(Operation operation)->std::invoke_result_t<Operation>{
        for(;;){
            try{
                if constexpr(std::is_void_v<std::invoke_result_t<Operation>>){operation();storageRecovered();return;}
                else {auto result=operation();storageRecovered();return result;}
            }catch(const ReportCapacityError&){throw;}
            catch(const std::exception& e){
                std::unique_lock lock(mutex);status.storageBlocked=true;
                status.message=std::string("本地缓存读写失败，保留当前任务并重试：")+e.what();
                if(stop)throw;
                cv.wait_for(lock,std::chrono::seconds(5),[&]{return stop;});
                if(stop)throw;
            }
        }
    }
    void run(){
        HANDLE file=INVALID_HANDLE_VALUE;
        try {
            std::filesystem::create_directories(dir);file=CreateFileW((dir/L"report.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("另一实例正在使用角色上报，请关闭其上报工具后重启");
            auto ownedStore=storage([&]{return std::make_unique<CharacterReportStore>(dir/L"character-report.db");});
            auto& store=*ownedStore;unsigned failures=0;int64_t nextAttempt=millis()+60000;
            while(true){
                CharacterReportConfig cfg;bool enabled,flush,retryNow,stopping;uint64_t gen;std::map<std::string,ReportJson> incoming;
                {std::unique_lock lock(mutex);cv.wait_for(lock,std::chrono::milliseconds(300));stopping=stop;cfg=config;enabled=status.enabled&&!status.authPaused;flush=std::exchange(force,false);retryNow=std::exchange(retry,false);gen=generation;incoming.swap(inbox);persisting=!incoming.empty();}
                if(cfg.url.empty()){if(stopping)break;continue;}
                auto target=targetKey(cfg);
                size_t duplicates=0,cacheFull=0;
                for(const auto& [key,snapshot]:incoming){
                    try{if(!storage([&]{return store.observe(target,snapshot.at("snapshot"),snapshot.at("time").get<int64_t>());}))++duplicates;}
                    catch(const ReportCapacityError&){++cacheFull;}
                }
                {std::lock_guard lock(mutex);persisting=false;status.cacheFull+=cacheFull;
                    if(status.storageBlocked){status.storageBlocked=false;status.message="本地缓存写入已恢复";}}
                if(stopping)break;
                if(retryNow){storage([&]{store.retry(target);});nextAttempt=0;failures=0;}
                const auto counts=storage([&]{return std::pair{store.count(target),store.count(target,true)};});
                {std::lock_guard lock(mutex);status.deduplicated+=duplicates;status.pending=counts.first;status.blocked=counts.second;}
                // An empty queue starts a fresh collection window; do not send a newly seen player immediately.
                if(counts.first==0){nextAttempt=std::max(nextAttempt,millis()+60000);continue;}
                if(!enabled || (!flush && millis()<nextAttempt))continue;
                auto batch=storage([&]{return store.pending(target,millis(),flush);});if(batch.empty())continue;
                {std::lock_guard lock(mutex);if(generation!=gen || !status.enabled)continue;status.uploading=true;status.message="正在上传角色资料";}
                try {
                    ReportJson records=ReportJson::array();for(const auto& item:batch){
                        ReportJson row={{"serverId",item.server},{"characterId",item.character}};
                        for(auto field=item.record.at("fields").begin();field!=item.record.at("fields").end();++field)row[field.key()]=field.value().at("value");
                        records.push_back(std::move(row));
                    }
                    auto response=postCharacterReport(cfg,ReportJson{{"characters",records}}.dump());
                    auto now=millis();size_t successes=0;std::string error;
                    if(response.status==200){
                        auto result=ReportJson::parse(response.body);
                        if(!result.value("ok",false) || !result.contains("received") || !result["received"].is_number_unsigned()
                            || result["received"].get<size_t>()!=batch.size() || !result.contains("written") || !result["written"].is_number_unsigned()
                            || result["written"].get<size_t>()!=batch.size())throw std::runtime_error("整批上传确认不完整，将重试");
                        for(const auto& sent:batch){storage([&]{store.acknowledge(target,sent,now);});++successes;}
                        failures=0;nextAttempt=now+60000;
                    }else{
                        const bool permanent=response.status>=400&&response.status<500&&response.status!=408&&response.status!=429&&response.status!=401&&response.status!=403;
                        auto delay=std::max<int64_t>(response.retryAfterMs,std::min<int64_t>(300000,5000LL<<std::min(++failures,6u)));
                        for(const auto& item:batch)storage([&]{store.fail(target,item,now,delay,permanent);});
                        nextAttempt=now+delay;error="上报失败 HTTP "+std::to_string(response.status);
                        if(response.status==401||response.status==403){std::lock_guard lock(mutex);if(generation==gen){status.authPaused=true;error+="，请检查上传令牌后重试";}}
                    }
                    {std::lock_guard lock(mutex);status.success+=successes;if(successes)status.lastSuccess=now;status.message=error.empty()?"本批角色已确认":error;}
                }catch(const std::exception& e){auto delay=std::min<int64_t>(300000,5000LL<<std::min(++failures,6u));for(const auto& item:batch)storage([&]{store.fail(target,item,millis(),delay,false);});nextAttempt=millis()+delay;std::lock_guard lock(mutex);status.message=e.what();}
                const auto remaining=storage([&]{return std::pair{store.count(target),store.count(target,true)};});
                {std::lock_guard lock(mutex);status.uploading=false;status.pending=remaining.first;status.blocked=remaining.second;}
            }
        }catch(const std::exception& e){std::lock_guard lock(mutex);fatal=true;status.enabled=false;status.uploading=false;status.message=std::string("上报已停止：")+e.what();}
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    }
};
CharacterReporter::CharacterReporter(std::filesystem::path dir):impl_(std::make_unique<Impl>(std::move(dir))){}
CharacterReporter::~CharacterReporter()=default;
CharacterReportConfig CharacterReporter::config()const{std::lock_guard lock(impl_->mutex);return impl_->config;}
CharacterReportStatus CharacterReporter::status()const{std::lock_guard lock(impl_->mutex);return impl_->status;}
void CharacterReporter::configure(CharacterReportConfig cfg,bool enabled){
    try {
        parseUrl(cfg.url);if(cfg.token.empty()||cfg.token.size()>4096||cfg.token.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("请输入有效的上传令牌");
        std::unique_lock lock(impl_->mutex);
        if(impl_->fatal)throw std::runtime_error("上报工作线程已停止，请检查本地数据库或其他实例后重启工具");
        if((!impl_->inbox.empty()||impl_->persisting)&&impl_->config.url!=cfg.url)throw std::runtime_error("请先暂停上报，等待本地缓存写入后再切换地址");
        std::filesystem::create_directories(impl_->dir);auto tmp=impl_->dir/L"report-config.tmp";
        {std::ofstream f(tmp,std::ios::binary|std::ios::trunc);f<<ReportJson{{"url",cfg.url},{"protectedToken",protect(cfg.token)}}.dump(2);f.flush();if(!f)throw std::runtime_error("无法保存上报设置");}
        if(!MoveFileExW(tmp.c_str(),(impl_->dir/L"report-config.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("无法替换上报设置");
        impl_->config=std::move(cfg);impl_->status.enabled=enabled;impl_->status.authPaused=false;impl_->status.message=enabled?"已启用，等待实时角色资料":"已暂停上报";++impl_->generation;
        lock.unlock();
        impl_->cv.notify_all();
    }catch(const std::exception& e){std::lock_guard lock(impl_->mutex);impl_->status.message=e.what();}
}
void CharacterReporter::observe(const NearbyObject& object){
    {std::lock_guard lock(impl_->mutex);if(!impl_->status.enabled || object.isSelf)return;}
    auto snapshot=reportObservation(object);if(!snapshot)return;auto key=snapshot->at("serverId").get<std::string>()+":"+snapshot->at("characterId").get<std::string>();
    std::lock_guard lock(impl_->mutex);if(impl_->inbox.size()>=10000&&!impl_->inbox.contains(key)){++impl_->status.overflow;return;}
    impl_->inbox[key]={{"snapshot",*snapshot},{"time",millis()}};
}
void CharacterReporter::flush(bool retry){std::lock_guard lock(impl_->mutex);impl_->force=true;impl_->retry|=retry;if(retry)impl_->status.authPaused=false;impl_->cv.notify_all();}
std::filesystem::path CharacterReporter::defaultDirectory(){wchar_t path[32768]{};auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",path,32768);if(n && n<32768)return std::filesystem::path(path)/L"Aion2Pipe"/L"report";throw std::runtime_error("无法确定本地数据目录");}
}
