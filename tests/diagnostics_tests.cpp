#include "diagnostics.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <thread>
int main(){
    auto root=std::filesystem::temp_directory_path()/(L"Aion2Pipe-log-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try {std::filesystem::create_directories(root);std::ofstream(root/L"blocked").put('x');
        aion::Diagnostics log;if(!log.initialize(root/L"blocked",root/L"中文路径",600))throw std::runtime_error("fallback failed");
        if(log.path().parent_path()!=root/L"中文路径")throw std::runtime_error("wrong fallback path");
        auto firstPath=log.path();for(int i=0;i<20;++i)log.write("diagnostic",std::to_string(i)+" 中文\nline\"quoted\" "+std::string(100,'x'));
        std::string error;if(log.exportTo(log.path(),error))throw std::runtime_error("active log overwrite allowed");
        if(!log.exportTo(root/L"export.log",error))throw std::runtime_error(error);
        std::ifstream input(root/L"export.log",std::ios::binary);std::string line;int last=-1,count=0;
        while(std::getline(input,line)){auto row=nlohmann::json::parse(line);auto details=row.at("details").get<std::string>();int sequence=std::stoi(details);if(sequence<=last)throw std::runtime_error("rotation order");last=sequence;++count;
            if(details.find("中文\nline\"quoted\"")==std::string::npos)throw std::runtime_error("unicode or newline damaged");}
        if(!count || last!=19 || count>=20)throw std::runtime_error("rotation or flush missing");
        if(!log.initialize(root/L"中文路径",{}))throw std::runtime_error("reinitialize failed");
        if(log.path()==firstPath)throw std::runtime_error("session filename reused");
        {
            aion::Diagnostics recovery;if(!recovery.initialize(root/L"recovery",{},256))throw std::runtime_error("recovery initialize");
            recovery.write("before",std::string(200,'a'));
            auto locked=recovery.path();locked+=L".2";std::ofstream(locked).put('x');
            HANDLE blocker=CreateFileW(locked.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(blocker==INVALID_HANDLE_VALUE)throw std::runtime_error("lock rotation fixture");
            recovery.write("blocked",std::string(200,'b'));
            CloseHandle(blocker);
            if(recovery.error().empty())throw std::runtime_error("rotation failure not reported");
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            recovery.write("recovered","after sharing violation");
            if(!recovery.error().empty())throw std::runtime_error("rotation did not recover");
            std::ifstream restored(recovery.path());std::string record;std::getline(restored,record);
            if(nlohmann::json::parse(record).at("event")!="recovered")throw std::runtime_error("recovered record missing");
        }
        {
            const auto dir=root/L"retention";std::filesystem::create_directories(dir);
            // This invalid PID cannot name a running Windows process.
            for(int i=1;i<=25;++i){char name[100];snprintf(name,sizeof(name),"Aion2Pipe-200001%02d-000000-000-4294967294-1.log",i);
                std::ofstream(dir/name).put('x');std::ofstream(dir/(std::string(name)+".1")).put('y');}
            auto live=dir/("Aion2Pipe-19990101-000000-000-"+std::to_string(GetCurrentProcessId())+"-1.log");std::ofstream(live).put('z');
            std::ofstream(dir/L"user-export.log").put('e');std::ofstream(dir/L"Aion2Pipe-custom.log").put('c');
            aion::Diagnostics retained;if(!retained.initialize(dir,{}))throw std::runtime_error("retention initialize");
            if(std::filesystem::exists(dir/L"Aion2Pipe-20000105-000000-000-4294967294-1.log") ||
               std::filesystem::exists(dir/L"Aion2Pipe-20000105-000000-000-4294967294-1.log.1"))throw std::runtime_error("old history retained");
            if(!std::filesystem::exists(dir/L"Aion2Pipe-20000106-000000-000-4294967294-1.log") ||
               !std::filesystem::exists(live) || !std::filesystem::exists(dir/L"user-export.log") ||
               !std::filesystem::exists(dir/L"Aion2Pipe-custom.log"))throw std::runtime_error("protected history removed");
            size_t files=0;for(const auto& entry:std::filesystem::directory_iterator(dir))++files;
            if(files!=44)throw std::runtime_error("incorrect retained session count");
        }
        aion::Diagnostics failed;if(failed.initialize(root/L"blocked",root/L"blocked") || failed.error().empty())throw std::runtime_error("unwritable paths not reported");
        std::cout<<"Logging fallback, Unicode, flush, rotation order, export protection and unique sessions passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
