#include "character_report.hpp"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <sqlite3.h>
using namespace aion;
static void check(bool v,const char* message){if(!v)throw std::runtime_error(message);}
int main(){try {
    check(reportHash("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256");
    NearbyObject o;o.kind=ObjectKind::Player;o.appearanceSeen=true;o.values={{"server_id","1005"},{"character_dbid","282882351594255517"},{"name","测试玩家"},{"level","30"},{"gender","2 / Female"},{"guild","公会"},{"combat_power","9007199254740992"}};
    auto snapshot=reportObservation(o);check(snapshot.has_value(),"player mapping");check(snapshot->at("values")["gender"]==2,"enum normalization");check(!snapshot->at("values").contains("combatPower"),"reject JS unsafe integer");
    o.isSelf=true;check(!reportObservation(o),"self excluded");o.isSelf=false;
    o.kind=ObjectKind::Npc;check(!reportObservation(o),"NPC excluded");o.kind=ObjectKind::Environment;check(!reportObservation(o),"environment excluded");o.kind=ObjectKind::Player;o.appearanceSeen=false;check(!reportObservation(o),"incomplete excluded");o.appearanceSeen=true;
    auto path=std::filesystem::temp_directory_path()/(L"aion-report-test-"+std::to_wstring(GetCurrentProcessId())+L".db");
    std::string client;
    {
        CharacterReportStore db(path);client=db.clientId();check(db.observe("one",*snapshot,1000),"first observation");check(!db.observe("one",*snapshot,2000),"pending dedup");check(db.pending("one",3999).empty(),"coalescing deadline");
        auto batch=db.pending("one",4000);check(batch.size()==1,"one pending");auto old=batch[0];
        (*snapshot)["values"]["level"]=31;check(db.observe("one",*snapshot,4100),"changed revision");db.acknowledge("one",old,4200);check(db.count("one")==1,"old ack must not acknowledge new version");
        auto next=db.pending("one",5000);check(next[0].revision==2,"monotonic revision");check(next[0].record["fields"]["characterName"]["observedAt"]==1000,"unchanged field keeps clock");check(next[0].record["fields"]["level"]["observedAt"]==4100,"changed field clock");
        db.fail("one",next[0],5000,10000,false);check(db.pending("one",14999).empty(),"backoff survives queue reads");db.acknowledge("one",next[0],15000);check(db.count("one")==0,"ack success");
        check(db.observe("two",*snapshot,16000),"destination isolated");
    }
    {
        CharacterReportStore db(path);check(db.clientId()==client,"stable client ID");check(!db.observe("one",*snapshot,20000),"restart dedup");check(db.count("one")==0,"no resend after restart");
        (*snapshot)["values"]["legionName"]=nullptr;db.observe("one",*snapshot,21000);auto b=db.pending("one",25000);check(b[0].record["fields"]["legionName"]["value"].is_null(),"explicit clear");
        db.fail("one",b[0],25000,0,true);check(db.count("one",true)==1&&db.pending("one",26000).empty(),"reject quarantined");db.retry("one");check(db.pending("one",26000).size()==1,"explicit retry");
    }
    {
        CharacterReportStore db(path);
        for(int i=0;i<201;++i){auto entry=*snapshot;entry["characterId"]=std::to_string(100000+i);db.observe("batch",entry,30000);}
        auto batch=db.pending("batch",34000);check(batch.size()==200,"maximum API batch is 200 players");
        for(const auto& item:batch)db.acknowledge("batch",item,35000);
        check(db.pending("batch",35000).size()==1,"overflow retained for next batch");
    }
    {
        // Simulate an existing installation with a large acknowledged history.
        sqlite3* raw{};auto utf=path.u8string();
        check(sqlite3_open(reinterpret_cast<const char*>(utf.c_str()),&raw)==SQLITE_OK,"open history fixture");
        check(sqlite3_exec(raw,"DROP INDEX reports_pending; WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<99000) INSERT INTO reports(target,server,character,payload,hash,acked,revision,due) SELECT 'batch','1005','history-'||x,'{}','same','same',1,0 FROM n",nullptr,nullptr,nullptr)==SQLITE_OK,"seed acknowledged history");
        sqlite3_close(raw);
        CharacterReportStore db(path); // Existing databases receive the index on reopen.
        check(db.count("batch")==1,"large history does not change pending count");
        check(db.pending("batch",35000).size()==1,"large history does not hide pending record");
        check(sqlite3_open(reinterpret_cast<const char*>(utf.c_str()),&raw)==SQLITE_OK,"open indexed fixture");
        for(const char* query:{
            "SELECT COUNT(*) FROM reports WHERE target='batch' AND hash!=acked AND blocked=0",
            "SELECT server,character,payload,hash,revision FROM reports WHERE target='batch' AND hash!=acked AND blocked=0 AND due<=35000 ORDER BY due LIMIT 200"}){
            sqlite3_stmt* statement{};
            check(sqlite3_prepare_v2(raw,query,-1,&statement,nullptr)==SQLITE_OK,"prepare indexed polling");
            int rc;while((rc=sqlite3_step(statement))==SQLITE_ROW){}
            check(rc==SQLITE_DONE,"finish indexed polling");
            check(sqlite3_stmt_status(statement,SQLITE_STMTSTATUS_VM_STEP,0)<500,"polling work must not scale with 99000 acknowledged rows");
            check(sqlite3_stmt_status(statement,SQLITE_STMTSTATUS_SORT,0)==0,"pending queue requires no temporary sort");
            sqlite3_finalize(statement);
        }
        sqlite3_stmt* total{};check(sqlite3_prepare_v2(raw,"SELECT COUNT(*) FROM reports",-1,&total,nullptr)==SQLITE_OK,"count fixture");
        check(sqlite3_step(total)==SQLITE_ROW,"read fixture count");auto remaining=100000-sqlite3_column_int(total,0);sqlite3_finalize(total);
        const auto fill="WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<"+std::to_string(remaining)+") INSERT INTO reports(target,server,character,payload,hash,acked,revision,due) SELECT 'batch','1005','filler-'||x,'{}','same','same',1,0 FROM n";
        check(sqlite3_exec(raw,fill.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK,"fill store to capacity");
        sqlite3_close(raw);
        auto newPlayer=*snapshot;newPlayer["characterId"]="999999999";
        bool full=false;try{db.observe("batch",newPlayer,35000);}catch(const ReportCapacityError&){full=true;}
        check(full,"new records at capacity are distinctly rejected");
        check(db.pending("batch",35000).size()==1,"capacity rejection preserves pending work");
        auto batch=db.pending("batch",35000);db.acknowledge("batch",batch[0],36000);
        check(db.count("batch")==0,"ack removes pending index entry");
        auto changed=*snapshot;changed["characterId"]="100000";changed["values"]["level"]=32;
        check(db.observe("batch",changed,37000),"existing player still updates at capacity");
        check(db.pending("batch",40000).size()==1,"existing update still queues at capacity");
    }
    std::filesystem::remove(path);std::cout<<"character report store tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
