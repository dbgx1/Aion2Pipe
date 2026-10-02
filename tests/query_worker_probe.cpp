#include "query_worker.hpp"
#include <windows.h>
#include <iostream>
#include <thread>
#include <fstream>
using namespace aion;
int wmain(int argc,wchar_t** argv){try{
    if(argc>1 && std::wstring(argv[1])==L"--embedded-config-test"){
        auto directory=std::filesystem::temp_directory_path()/(L"aion2-query-config-"+std::to_wstring(GetCurrentProcessId()));
        QueryProxy proxy;QueryWorker first(proxy,directory);auto original=first.config();
        if(original.clientId.empty() || original.password.empty())throw std::runtime_error("Missing embedded credentials on clean installation");
        {std::ofstream f(directory/L"settings.json");f<<R"({"clientId":"external-override","passwordDpapi":"invalid","boot":7})";}
        QueryWorker second(proxy,directory);auto loaded=second.config();
        if(loaded.clientId!=original.clientId || loaded.password!=original.password)throw std::runtime_error("External settings changed embedded credentials");
        std::filesystem::remove(directory/L"settings.json");std::filesystem::remove(directory);
        std::cout<<"Embedded credentials work without external settings; legacy overrides ignored. No network connection made.\n";return 0;
    }
    if(argc<2)throw std::runtime_error("Usage: query_worker_probe settings-directory");
    QueryProxy emptyProxy; // Never starts or attaches to a game connection.
    QueryWorker worker(emptyProxy,std::filesystem::path(argv[1]));auto config=worker.config();worker.start(config);
    auto deadline=GetTickCount64()+10000;
    while(GetTickCount64()<deadline && !worker.status().connected)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if(!worker.status().connected)throw std::runtime_error(worker.status().message);
    if(worker.status().ready)throw std::runtime_error("Worker advertised readiness without a game connection");
    if(argc>2 && std::wstring(argv[2])==L"--tasks"){
        std::cout<<"TASK_TOPIC=aion2/query-workers/"<<config.clientId<<'/'<<worker.status().sessionId<<"/task"<<std::endl;
        const auto taskDeadline=GetTickCount64()+45000;
        while(GetTickCount64()<taskDeadline){
            const auto state=worker.status();
            if(state.rejected>=2 && state.duplicates>=1 && state.failed>=1){
                if(state.rejected!=2 || state.duplicates!=1 || state.failed!=1 || state.busy || state.completed || state.lastRejection!="game_session_changed")
                    throw std::runtime_error("Unexpected task validation outcome");
                worker.stop();std::cout<<"PASS: expired task rejected, duplicate suppressed, stale game session rejected, invalid identifier rejected\n";return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        throw std::runtime_error("Task validation probe timed out");
    }
    QueryWorker second(emptyProxy,std::filesystem::path(argv[1]));bool exclusive=false;
    try{second.start(config);}catch(const std::exception&){exclusive=true;}
    if(!exclusive)throw std::runtime_error("Duplicate local worker acquired same identity");
    std::cout<<"Connected with dedicated device credential; idle without game; duplicate local process rejected\n";
    // Keep the client online briefly for broker/webhook inspection and MQTT keepalive.
    deadline=GetTickCount64()+20000;
    while(GetTickCount64()<deadline){if(!worker.status().connected)throw std::runtime_error(worker.status().message);std::this_thread::sleep_for(std::chrono::milliseconds(100));}
    const auto before=GetTickCount64();worker.stop();
    if(GetTickCount64()-before>3000)throw std::runtime_error("Worker cancellation exceeded 3 seconds");
    if(worker.status().enabled || worker.status().connected)throw std::runtime_error("Worker failed to stop");
    std::cout<<"Keepalive remained connected; cancellation completed without affecting game proxy\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
