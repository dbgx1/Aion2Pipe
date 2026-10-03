#include "query_worker.hpp"
#include "query_credential.hpp"
#include "reconnect_backoff.hpp"
#include "query_history.hpp"
#include "query_batch.hpp"
#include "diagnostics.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <charconv>
#include <fstream>
#include <map>
#include <thread>
#include <utility>
namespace aion {
namespace {
using Json=nlohmann::json;
int64_t nowMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
bool identifier(const std::string& value){return !value.empty() && value.size()<=64 && std::all_of(value.begin(),value.end(),[](unsigned char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_';});}
std::string randomId(){
    std::array<uint8_t,16> bytes{};if(BCryptGenRandom(nullptr,bytes.data(),ULONG(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("无法生成查询客户端标识");
    std::string out;for(auto b:bytes){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;
}
void save(const std::filesystem::path& path,const Json& json){
    auto temp=path;temp+=L".tmp";const auto data=json.dump(2);
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("无法保存查询服务设置");
    DWORD written=0;bool ok=WriteFile(file,data.data(),DWORD(data.size()),&written,nullptr) && written==data.size() && FlushFileBuffers(file);CloseHandle(file);
    if(!ok || !MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("无法提交查询服务设置");
}
uint64_t decimal(const Json& task,const char* name,uint64_t max){
    const auto text=task.at(name).get<std::string>();uint64_t value=0;
    if(text.empty() || text.size()>20 || text[0]=='0')throw std::invalid_argument("Invalid target identifier");
    const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
    if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || !value || value>max)throw std::invalid_argument("Invalid target identifier");return value;
}
struct Record {std::string batchId;Json task,event;int64_t expires{};uint16_t packetId{};bool acknowledged{};};
}
struct QueryWorker::Impl {
    QueryProxy& proxy;std::filesystem::path directory;
    mutable std::mutex mutex;QueryWorkerConfig config;QueryWorkerStatus status;
    std::atomic<bool> stopping=true,stopRequested=true,finished=true;std::thread worker;HANDLE lease=INVALID_HANDLE_VALUE;
    uint64_t boot=0;
    Impl(QueryProxy& p,std::filesystem::path dir):proxy(p),directory(std::move(dir)){
        std::filesystem::create_directories(directory);
        auto path=directory/L"settings.json";
        if(std::filesystem::exists(path)){
            if(std::filesystem::file_size(path)>16384)throw std::runtime_error("查询设置文件过大");
            std::ifstream file(path);auto j=Json::parse(file);boot=j.value("boot",uint64_t(0));
        }
        config.clientId=embedded_query::clientId;config.password=embedded_query::password;
        if(!identifier(config.clientId))throw std::runtime_error("查询客户端标识无效");
    }
    void run(QueryWorkerConfig settings,uint64_t generation){
        const auto session=randomId();const std::string base="aion2/query-workers/"+settings.clientId+"/"+session;
        {std::lock_guard lock(mutex);status.sessionId=session;}
        MqttConfig mqttConfig;mqttConfig.clientId="query-"+settings.clientId+"-"+session;mqttConfig.username="query-"+settings.clientId;mqttConfig.password=std::move(settings.password);mqttConfig.topic=base+"/task";
        QueryHistory<Record> history;auto& records=history.entries;
        QueryHistory<QueryBatch> batchHistory;auto& batches=batchHistory.entries;std::string runningBatch;
        std::map<std::string,std::pair<int64_t,std::set<std::string>>> cancellations;
        std::shared_ptr<QueryReceipt> active;std::string activeId,gameSession="none";
        uint32_t activeServer=0;
        size_t connectionId=0;uint64_t seq=0;int64_t nextState=0,nextInspect=0;std::string previousState;
        // Spread heartbeats across devices even when they all have boot == 1.
        uint32_t heartbeatHash=2166136261u;
        for(unsigned char c:settings.clientId)heartbeatHash=(heartbeatHash^c)*16777619u;
        const int64_t heartbeatInterval=55000+heartbeatHash%5000;
        ReconnectBackoff backoff;
        while(!stopping && !stopRequested){
            uint64_t connectedAt=0;
            try{
                MqttTransport mqtt(mqttConfig,stopping);connectedAt=GetTickCount64();previousState.clear();nextState=0;
                for(auto& [id,r]:records)r.packetId=0;
                {std::lock_guard lock(mutex);status.connected=true;status.message="已连接调度器，等待游戏查询连接";}
                while(!stopping && !stopRequested){
                    const auto now=nowMs();
                    history.prune(now,activeId);
                    batchHistory.prune(now,active && !records.at(activeId).batchId.empty()?records.at(activeId).batchId:runningBatch);
                    for(auto it=cancellations.begin();it!=cancellations.end();)if(it->second.first<=now)it=cancellations.erase(it);else ++it;
                    if(active){
                        const auto outcome=active->snapshot();
                        auto& r=records.at(activeId);
                        if(outcome.phase==QueryPhase::Completed || outcome.phase==QueryPhase::Failed || now>=r.expires){
                            const bool completed=outcome.phase==QueryPhase::Completed && now<r.expires;
                            r.event={{"type",completed?"completed":"failed"},{"taskId",r.task.at("taskId")},{"attemptId",activeId},{"gameSessionId",r.task.at("gameSessionId")},
                                {"status",completed?outcome.presence:"unknown"}};
                            if(!completed){
                                const auto error=now>=r.expires?std::string(r.batchId.empty()?"task_expired":"batch_timeout"):outcome.error;r.event["error"]=error;
                                diagnostics().write("query_worker_failed","connection="+std::to_string(connectionId)+" reason="+error);
                                std::lock_guard lock(mutex);status.lastError=error;
                            }
                            r.packetId=0;r.acknowledged=false;
                            if(!r.batchId.empty())batches.at(r.batchId).add(r.event,now);
                            {std::lock_guard lock(mutex);if(completed)++status.completed;else ++status.failed;status.busy=false;status.target.clear();}
                            active.reset();activeId.clear();nextInspect=0;
                        }
                    }
                    if(!runningBatch.empty()){
                        auto& batch=batches.at(runningBatch);
                        if(now>=batch.expires)batch.fail("batch_timeout",now);
                        if(batch.done()){
                            runningBatch.clear();nextInspect=0;
                        }else if(!active){
                            const auto live=proxy.connections();const auto current=std::find_if(live.begin(),live.end(),[&](const QueryConnection& view){return view.id==connectionId;});
                            if(current==live.end() || !current->open || !current->ready || current->serverId!=activeServer || batch.task.at("gameSessionId")!=gameSession){
                                batch.fail("game_session_changed",now);
                            }else if(current->queryAvailable){
                                const auto task=batch.next();const auto attempt=task.at("attemptId").get<std::string>();
                                Record record;record.task=task;record.expires=batch.expires;record.batchId=runningBatch;
                                records.emplace(attempt,std::move(record));
                                active=proxy.requestTracked(connectionId,uint32_t(decimal(task,"serverId",65535)),decimal(task,"characterId",0x7fffffffffffffffULL));
                                if(active){activeId=attempt;std::lock_guard lock(mutex);status.busy=true;status.target=std::to_string(batch.results.size()+1)+" / "+std::to_string(batch.task.at("tasks").size());}
                                else batch.fail("game_connection_busy",now);
                            }
                        }
                    }
                    if(now>=nextInspect){
                        auto views=proxy.connections();const QueryConnection* chosen=nullptr;
                        for(const auto& view:views)if(view.id==connectionId && view.open && view.ready){chosen=&view;break;}
                        if(!active && runningBatch.empty() && (!chosen || !chosen->queryAvailable))for(const auto& view:views)if(view.queryAvailable){chosen=&view;break;}
                        if(!chosen && !active && runningBatch.empty())for(const auto& view:views)if(view.open && view.ready){chosen=&view;break;}
                        if(!active && runningBatch.empty()){
                            connectionId=chosen?chosen->id:0;activeServer=chosen?chosen->serverId:0;
                            gameSession=chosen && activeServer?"g-"+std::to_string(chosen->id)+"-s-"+std::to_string(activeServer):"none";
                        }
                        const bool ready=chosen && chosen->queryAvailable && !active && runningBatch.empty() && history.entries.size()+50<QueryHistory<Record>::Capacity && batchHistory.ready();
                        const auto cooldown=chosen?chosen->queryCooldownMs:0;
                        const auto stateKey=gameSession+":"+std::to_string(activeServer)+(ready?":ready":":busy");
                        // Do not publish a heartbeat for every decreasing cooldown value.
                        if(stateKey!=previousState || now>=nextState){
                            mqtt.publish(base+"/state",Json{{"clientId",settings.clientId},{"sessionId",session},{"gameSessionId",gameSession},{"serverId",activeServer?Json(std::to_string(activeServer)):Json(nullptr)},{"boot",generation},{"seq",++seq},{"ready",ready},{"cooldownMs",cooldown},{"batchSize",50}}.dump());
                            previousState=stateKey;nextState=now+heartbeatInterval;
                        }
                        {std::lock_guard lock(mutex);status.ready=ready;status.serverId=activeServer?std::to_string(activeServer):"";status.busy=bool(active)||!runningBatch.empty();status.message=(active || !runningBatch.empty())?"正在执行控制台查询":ready?"可领取同区服查询任务":!history.ready()?"任务记录暂满，等待过期记录清理":chosen && !chosen->serverId?"等待识别当前游戏区服":chosen?"等待当前查询结束":"等待游戏查询连接";}
                        nextInspect=now+250;
                    }
                    for(auto& [id,r]:records)if(r.batchId.empty() && !r.event.is_null() && !r.acknowledged && !r.packetId && now<r.expires+10000){
                        r.packetId=mqtt.publish(base+"/events",r.event.dump());break;
                    }
                    for(auto& [id,batch]:batches)if(batch.done() && !batch.acknowledged && now>=batch.nextPublish && now<batch.expires+10000){
                        mqtt.publish(base+"/events",batch.report().dump());batch.nextPublish=now+2000;break;
                    }
                    if(auto publication=mqtt.poll(100)){
                        if(stopRequested){mqtt.acknowledge(publication->id);continue;}
                        if(publication->topic!=mqttConfig.topic || publication->retained){mqtt.acknowledge(publication->id);continue;}
                        Json task,overflowEvent;std::string attempt;
                        try{
                            task=Json::parse(publication->payload);
                            const auto type=task.at("type").get<std::string>();
                            if(type=="query_result_ack"){
                                if(task.size()!=2 || !identifier(task.at("batchId").get<std::string>()))throw std::invalid_argument("Invalid acknowledgement");
                                if(auto it=batches.find(task.at("batchId").get<std::string>());it!=batches.end())it->second.acknowledged=true;
                                mqtt.acknowledge(publication->id);continue;
                            }
                            if(type=="cancel_query_tasks"){
                                const auto id=task.at("batchId").get<std::string>();
                                if(task.size()!=3 || !identifier(id) || !task.at("attemptIds").is_array() || task.at("attemptIds").size()>50)throw std::invalid_argument("Invalid cancellation");
                                std::set<std::string> cancelled;
                                for(const auto& value:task.at("attemptIds")){auto target=value.get<std::string>();if(!identifier(target))throw std::invalid_argument("Invalid cancellation");cancelled.insert(target);}
                                if(auto it=batches.find(id);it!=batches.end())it->second.fail("cancelled",now,&cancelled);
                                else if(cancellations.size()<4096){auto& pending=cancellations[id];pending.first=now+110000;pending.second.insert(cancelled.begin(),cancelled.end());}
                                mqtt.acknowledge(publication->id);continue;
                            }
                            if(type=="query_players_online"){
                                const auto received=nowMs();QueryBatch batch(task,received);
                                const auto id=task.at("batchId").get<std::string>();
                                if(!identifier(id) || !identifier(task.at("gameSessionId").get<std::string>()))throw std::invalid_argument("Invalid batch identity");
                                for(const auto& item:task.at("tasks")){
                                    if(!identifier(item.at("attemptId").get<std::string>()) || !identifier(item.at("taskId").get<std::string>()))throw std::invalid_argument("Invalid task identity");
                                    decimal(item,"serverId",65535);decimal(item,"characterId",0x7fffffffffffffffULL);
                                }
                                if(auto it=batches.find(id);it!=batches.end()){
                                    if(it->second.task!=task)throw std::invalid_argument("Batch identity changed");
                                    it->second.acknowledged=false;it->second.nextPublish=0;std::lock_guard lock(mutex);++status.duplicates;
                                }else{
                                    if(batchHistory.full())throw std::invalid_argument("Batch history full");
                                    std::string rejection;
                                    if(batch.expires<=received)rejection="batch_timeout";
                                    else if(active || !runningBatch.empty() || records.size()+task.at("tasks").size()>=QueryHistory<Record>::Capacity)rejection="client_busy";
                                    else if(task.at("gameSessionId")!=gameSession || !connectionId)rejection="game_session_changed";
                                    for(const auto& item:task.at("tasks")){
                                        if(decimal(item,"serverId",65535)!=activeServer)rejection="server_mismatch";
                                        if(records.contains(item.at("attemptId").get<std::string>()))throw std::invalid_argument("Attempt already used");
                                    }
                                    if(!rejection.empty())batch.fail(rejection,received);
                                    if(auto cancelled=cancellations.find(id);cancelled!=cancellations.end()){batch.fail("cancelled",received,&cancelled->second.second);cancellations.erase(cancelled);}
                                    const bool done=batch.done();batches.emplace(id,std::move(batch));if(!done)runningBatch=id;
                                    nextInspect=0;
                                }
                                mqtt.acknowledge(publication->id);continue;
                            }
                            attempt=task.at("attemptId").get<std::string>();
                            if(!task.is_object() || task.size()!=7 || task.at("type")!="query_player_online" || !identifier(attempt) ||
                               !identifier(task.at("taskId").get<std::string>()) || !identifier(task.at("gameSessionId").get<std::string>()) || !task.at("expiresAt").is_number_integer())throw std::invalid_argument("Invalid task");
                            const auto expires=task.at("expiresAt").get<int64_t>();const auto received=nowMs();
                            const auto server=decimal(task,"serverId",65535),character=decimal(task,"characterId",0x7fffffffffffffffULL);
                            if(auto found=records.find(attempt);found!=records.end()){
                                if(found->second.task!=task)throw std::invalid_argument("Task identity changed");
                                if(!found->second.event.is_null()){found->second.packetId=0;found->second.acknowledged=false;}
                                {std::lock_guard lock(mutex);++status.duplicates;}
                            }else if(history.full()){
                                // No game request is executed without dedup space. Report overload
                                // explicitly; a failed publish must reconnect before PUBACK.
                                overflowEvent={{"type","rejected"},{"taskId",task.at("taskId")},{"attemptId",attempt},{"gameSessionId",task.at("gameSessionId")},{"status","unknown"},{"error","client_busy"}};
                                {std::lock_guard lock(mutex);++status.rejected;status.lastRejection="task_history_full";}
                                nextInspect=0;
                            }else{
                                Record record;record.task=task;record.expires=std::clamp(expires,received-180000,received+30000);
                                // Reserve deduplication state before a game request can be sent.
                                auto& saved=records.emplace(attempt,std::move(record)).first->second;
                                std::string rejection;
                                if(expires<=received || expires>received+30000)rejection="task_expired";
                                else if(task.at("gameSessionId")!=gameSession || !connectionId)rejection="game_session_changed";
                                else if(active || !runningBatch.empty())rejection="client_busy";
                                else if(server!=activeServer)rejection="server_mismatch";
                                else {
                                    const auto live=proxy.connections();const auto current=std::find_if(live.begin(),live.end(),[&](const QueryConnection& view){return view.id==connectionId;});
                                    if(current==live.end() || !current->queryAvailable || current->serverId!=server)rejection="server_mismatch";
                                    else {active=proxy.requestTracked(connectionId,uint32_t(server),character);if(!active)rejection="game_connection_busy";}
                                }
                                if(!rejection.empty()){
                                    saved.event={{"type","rejected"},{"taskId",task.at("taskId")},{"attemptId",attempt},{"gameSessionId",task.at("gameSessionId")},{"status","unknown"},{"error",rejection}};
                                    std::lock_guard lock(mutex);++status.rejected;status.lastRejection=rejection;
                                }
                                else{activeId=attempt;std::lock_guard lock(mutex);status.busy=true;status.target=std::to_string(server)+" / "+std::to_string(character);}
                                nextInspect=0;
                            }
                        }catch(const std::exception& e){
                            diagnostics().write("query_task_invalid",e.what());
                            std::lock_guard lock(mutex);++status.failed;status.lastError=e.what();status.message="已忽略无效或冲突的查询任务";
                        }
                        if(!overflowEvent.is_null())mqtt.publish(base+"/events",overflowEvent.dump());
                        mqtt.acknowledge(publication->id);
                    }
                    for(auto ack:mqtt.acknowledgements())for(auto& [id,r]:records)if(r.packetId==ack){r.acknowledged=true;r.packetId=0;}
                }
                if(stopRequested && !stopping){
                    // Best effort, bounded grace period. Network cancellation stays
                    // independent so an unreachable broker cannot hold up shutdown.
                    const auto finalState=mqtt.publish(base+"/state",Json{{"clientId",settings.clientId},{"sessionId",session},{"gameSessionId",gameSession},{"serverId",activeServer?Json(std::to_string(activeServer)):Json(nullptr)},{"boot",generation},{"seq",++seq},{"ready",false},{"cooldownMs",0}}.dump());
                    const auto deadline=GetTickCount64()+200;
                    bool acknowledged=false;
                    while(!stopping && !acknowledged && GetTickCount64()<deadline){
                        if(auto ignored=mqtt.poll(25))mqtt.acknowledge(ignored->id);
                        for(auto id:mqtt.acknowledgements())if(id==finalState)acknowledged=true;
                    }
                }
            }catch(const std::exception& e){
                {std::lock_guard lock(mutex);status.connected=status.ready=false;if(!stopping && !stopRequested){++status.reconnects;status.message=std::string("查询通信中断，将重连：")+e.what();}}
                const auto tick=GetTickCount64();
                const auto delay=backoff.failed(connectedAt?tick-connectedAt:0,heartbeatHash^uint32_t(tick)^uint32_t(seq));
                const auto deadline=tick+delay;
                while(!stopping && !stopRequested){const auto current=GetTickCount64();if(current>=deadline)break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(100,deadline-current)));}

            }
        }
        SecureZeroMemory(mqttConfig.password.data(),mqttConfig.password.size());
    }
};
QueryWorker::QueryWorker(QueryProxy& proxy,std::filesystem::path directory):impl_(std::make_unique<Impl>(proxy,std::move(directory))){}
QueryWorker::~QueryWorker(){stop();}
QueryWorkerConfig QueryWorker::config()const{std::lock_guard lock(impl_->mutex);return impl_->config;}
QueryWorkerStatus QueryWorker::status()const{std::lock_guard lock(impl_->mutex);return impl_->status;}
void QueryWorker::stop(){
    auto& p=*impl_;p.stopRequested=true;
    if(p.worker.joinable()){
        for(unsigned i=0;i<5 && !p.finished;++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    p.stopping=true;if(p.worker.joinable())p.worker.join();
    if(p.lease!=INVALID_HANDLE_VALUE){CloseHandle(p.lease);p.lease=INVALID_HANDLE_VALUE;}
    std::lock_guard lock(p.mutex);p.status.enabled=p.status.connected=p.status.ready=p.status.busy=false;p.status.target.clear();p.status.serverId.clear();p.status.message="查询服务已停止";
}
void QueryWorker::start(QueryWorkerConfig config){
    config.clientId=embedded_query::clientId;config.password=embedded_query::password;
    if(!identifier(config.clientId) || config.password.empty() || config.password.size()>4096)throw std::invalid_argument("内置查询凭据无效，请更新程序");
    stop();auto& p=*impl_;
    p.lease=CreateFileW((p.directory/L"service.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(p.lease==INVALID_HANDLE_VALUE)throw std::runtime_error("同一客户端标识已由其他程序使用");
    try{
        // Reread while holding the exclusive lease: another process may have advanced it.
        auto path=p.directory/L"settings.json";if(std::filesystem::exists(path)){std::ifstream f(path);auto j=Json::parse(f);p.boot=std::max(p.boot,j.value("boot",uint64_t(0)));}
        if(p.boot>=9007199254740990ULL)throw std::runtime_error("查询会话计数已超过限制");++p.boot;
        save(path,Json{{"clientId",config.clientId},{"boot",p.boot}});
        {std::lock_guard lock(p.mutex);p.config=config;p.status.enabled=true;p.status.message="正在连接查询调度器";}
        p.stopping=false;p.stopRequested=false;p.finished=false;
        p.worker=std::thread([&p,config=std::move(config)](){try{p.run(config,p.boot);}catch(const std::exception&){std::lock_guard lock(p.mutex);p.status.enabled=p.status.connected=p.status.ready=false;p.status.message="查询服务初始化失败";}p.finished=true;});
    }catch(...){stop();throw;}
}
}
