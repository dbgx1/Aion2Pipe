#include "character_report.hpp"
#include <windows.h>
#include <sqlite3.h>
#include <thread>
#include <iostream>
#include "sqlite_write_fault.hpp"
using namespace aion;
template<class Predicate>void waitFor(Predicate predicate){
    const auto until=GetTickCount64()+12000;
    while(!predicate()){if(GetTickCount64()>=until)throw std::runtime_error("reporter storage wait timed out");std::this_thread::sleep_for(std::chrono::milliseconds(30));}
}
int main(int argc,char** argv){try{
    sqlite_fault::install();
    const auto dir=std::filesystem::temp_directory_path()/(L"Aion2Pipe-store-fault-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    CharacterReporter reporter(dir);CharacterReportConfig config{argc>1?argv[1]:"http://127.0.0.1:1",std::string(40,'x')};reporter.configure(config,true);
    NearbyObject player;player.kind=ObjectKind::Player;player.appearanceSeen=true;
    player.values={{"server_id","1005"},{"character_dbid","123"},{"name","Storage fixture"},{"level","30"}};
    reporter.observe(player);waitFor([&]{return reporter.status().pending==1;});
    sqlite3* database{};const auto path=(dir/L"character-report.db").u8string();
    if(sqlite3_open(reinterpret_cast<const char*>(path.c_str()),&database)!=SQLITE_OK)throw std::runtime_error("open fault fixture");
    struct Close {sqlite3* db;~Close(){sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(db);}} close{database};
    if(sqlite3_exec(database,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("acquire write lock");
    player.values["character_dbid"]="456";reporter.observe(player);
    waitFor([&]{return reporter.status().storageBlocked;});
    if(!reporter.status().enabled)throw std::runtime_error("temporary database lock killed worker");
    reporter.configure({"http://localhost:1",config.token},true);
    if(reporter.config().url!=config.url)throw std::runtime_error("destination changed while unsaved batch pending");
    // A newer observation arrives while the old batch is being retried.
    player.values["level"]="31";reporter.observe(player);
    if(sqlite3_exec(database,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("release write lock");
    waitFor([&]{return !reporter.status().storageBlocked && reporter.status().pending==2;});
    waitFor([&]{
        sqlite3_stmt* statement{};sqlite3_prepare_v2(database,"SELECT payload FROM reports WHERE character='456'",-1,&statement,nullptr);
        bool updated=false;if(sqlite3_step(statement)==SQLITE_ROW){auto text=reinterpret_cast<const char*>(sqlite3_column_text(statement,0));updated=ReportJson::parse(text)["level"]["value"]==31;}
        sqlite3_finalize(statement);return updated;
    });
    if(!reporter.status().enabled || reporter.status().success)throw std::runtime_error("incorrect recovery/upload state");
    size_t expected=2;
    for(const auto fault:{SQLITE_FULL,SQLITE_IOERR_WRITE}){
        const auto previous=sqlite_fault::failures.load();sqlite_fault::errorCode=fault;
        player.values["character_dbid"]=std::to_string(1000+expected);reporter.observe(player);
        waitFor([&]{return reporter.status().storageBlocked && sqlite_fault::failures.load()>previous;});
        if(!reporter.status().enabled || reporter.status().success)throw std::runtime_error("write fault killed reporter or fabricated upload success");
        sqlite_fault::errorCode=SQLITE_OK;
        ++expected;waitFor([&]{return !reporter.status().storageBlocked && reporter.status().pending==expected;});
    }
    sqlite3_stmt* integrity{};sqlite3_prepare_v2(database,"PRAGMA integrity_check",-1,&integrity,nullptr);
    const bool intact=sqlite3_step(integrity)==SQLITE_ROW && std::string(reinterpret_cast<const char*>(sqlite3_column_text(integrity,0)))=="ok";
    sqlite3_finalize(integrity);if(!intact)throw std::runtime_error("database damaged after write fault");
    std::cout<<"PASS: SQLite VFS FULL and IOERR_WRITE faults, rollback, recovery and integrity_check\n";
    if(argc>1){
        sqlite_fault::errorCode=SQLITE_FULL;reporter.flush();
        waitFor([&]{return reporter.status().storageBlocked;});
        if(reporter.status().success || !reporter.status().enabled)throw std::runtime_error("local ack failure reported success or stopped worker");
        sqlite_fault::errorCode=SQLITE_OK;
        waitFor([&]{return reporter.status().success==expected && reporter.status().pending==0;});
        std::cout<<"PASS: HTTP success retained across local ACK write failure\n";
    }
    std::cout<<"PASS: real SQLite writer lock, bounded retry, destination protection, pending retention and latest observation recovery\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
