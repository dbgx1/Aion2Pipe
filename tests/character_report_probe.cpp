#include "character_report.hpp"
#include <windows.h>
#include <chrono>
#include <thread>
#include <iostream>
using namespace aion;
static void wait(CharacterReporter& reporter,size_t count){for(int i=0;i<850;++i){auto s=reporter.status();if(s.success>=count)return;std::this_thread::sleep_for(std::chrono::milliseconds(100));}throw std::runtime_error(reporter.status().message);}
int main(int argc,char** argv){try{if(argc!=3)return 2;
    auto dir=std::filesystem::path(argv[2]);CharacterReportConfig cfg{argv[1],std::string(40,'x')};
    NearbyObject player;player.kind=ObjectKind::Player;player.appearanceSeen=true;player.values={{"server_id","1005"},{"character_dbid","282882351594255517"},{"name","上报联调玩家"},{"level","30"}};
    {
        CharacterReporter reporter(dir);reporter.configure(cfg,true);reporter.observe(player);std::this_thread::sleep_for(std::chrono::seconds(5));
        if(reporter.status().success!=0)throw std::runtime_error("automatic upload did not collect a batch");
        wait(reporter,1);
        for(int i=0;i<10;++i)reporter.observe(player);std::this_thread::sleep_for(std::chrono::milliseconds(600));
        if(reporter.status().success!=1)throw std::runtime_error("duplicate upload");
        player.values["level"]="31";reporter.observe(player);reporter.flush();wait(reporter,2);
    }
    {
        CharacterReporter reporter(dir);if(reporter.config().token!=cfg.token)throw std::runtime_error("DPAPI roundtrip");
        reporter.configure(cfg,true);reporter.observe(player);reporter.flush();std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if(reporter.status().success!=0||reporter.status().pending!=0)throw std::runtime_error("restart dedup");
    }
    std::cout<<"HTTP + reporter + persistence integration passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
