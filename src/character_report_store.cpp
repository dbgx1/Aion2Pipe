#include "character_report.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <sqlite3.h>
#include <charconv>
#include <stdexcept>
#include <algorithm>
namespace aion {
namespace {
struct Statement {
    sqlite3* db;sqlite3_stmt* s{};
    Statement(sqlite3* d,const char* sql):db(d){if(sqlite3_prepare_v2(d,sql,-1,&s,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(d));}
    ~Statement(){sqlite3_finalize(s);}
    void bind(int i,std::string_view v){if(sqlite3_bind_text(s,i,v.data(),int(v.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw std::runtime_error("数据库绑定失败");}
    void bind(int i,int64_t n){sqlite3_bind_int64(s,i,n);}
    bool step(){auto rc=sqlite3_step(s);if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));return rc==SQLITE_ROW;}
    std::string text(int i){auto t=sqlite3_column_text(s,i);return t?reinterpret_cast<const char*>(t):"";}
    int64_t number(int i){return sqlite3_column_int64(s,i);}
};
void sql(sqlite3* db,const char* text){if(sqlite3_exec(db,text,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
}
std::string reportHash(std::string_view value){
    unsigned char hash[32]{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,(PUCHAR)value.data(),ULONG(value.size()),hash,sizeof(hash))<0)throw std::runtime_error("SHA256 failed");
    static const char* hex="0123456789abcdef";std::string result;for(auto c:hash){result+=hex[c>>4];result+=hex[c&15];}return result;
}
std::optional<ReportJson> reportObservation(const NearbyObject& o){
    if(o.isSelf || !o.present || !o.appearanceSeen || (o.kind!=ObjectKind::Player && o.kind!=ObjectKind::Mimic))return {};
    auto get=[&](const char* k){auto i=o.values.find(k);return i==o.values.end()?std::string{}:i->second;};
    auto digits=[](const std::string& v){return !v.empty() && v.size()<=20 && v[0]!='0' && v.find_first_not_of("0123456789")==std::string::npos;};
    auto server=get("server_id"),id=get("character_dbid"),name=get("name");
    if(!digits(server)||!digits(id)||name.empty()||name=="—")return {};
    ReportJson f={{"characterName",name}};
    auto guild=get("guild");if(o.values.contains("guild") && guild!="—")f["legionName"]=guild;
    auto race=get("race");if(race=="1 / Light")f["faction"]="天族";else if(race=="2 / Dark")f["faction"]="魔族";
    for(const auto& [from,to,max]:std::vector<std::tuple<const char*,const char*,uint64_t>>{
        {"level","level",999},{"equipment_level","equipItemLevel",2147483647},{"combat_power","combatPower",9007199254740991ULL},{"gender","gender",2}}){
        auto v=get(from);uint64_t n{};auto [p,ec]=std::from_chars(v.data(),v.data()+v.size(),n);
        if(ec==std::errc{} && (p==v.data()+v.size() || std::string_view(p,v.data()+v.size()-p).starts_with(" / ")) && n<=max)f[to]=n;
    }
    // Do not guess class from PC template ID, or clear an absent guild field.
    return ReportJson{{"serverId",server},{"characterId",id},{"values",f}};
}
CharacterReportStore::CharacterReportStore(const std::filesystem::path& path){
    auto utf=path.u8string();if(sqlite3_open_v2(reinterpret_cast<const char*>(utf.c_str()),&db_,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK){if(db_)sqlite3_close(db_);db_=nullptr;throw std::runtime_error("无法打开角色上报数据库");}
    try {
        sqlite3_busy_timeout(db_,1500);sql(db_,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA max_page_count=32768;");
        sql(db_,"CREATE TABLE IF NOT EXISTS settings(k TEXT PRIMARY KEY,v TEXT NOT NULL); CREATE TABLE IF NOT EXISTS reports(target TEXT,server TEXT,character TEXT,payload TEXT NOT NULL,hash TEXT NOT NULL,acked TEXT NOT NULL DEFAULT '',revision INTEGER NOT NULL,due INTEGER NOT NULL,blocked INTEGER NOT NULL DEFAULT 0,last_success INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(target,server,character));");
        // Keep polling cost proportional to pending work, not the lifetime dedup history.
        sql(db_,"CREATE INDEX IF NOT EXISTS reports_pending ON reports(target,blocked,due) WHERE hash!=acked;");
        auto random=reportHash(std::to_string(GetTickCount64())+std::to_string(GetCurrentProcessId())+std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        Statement s(db_,"INSERT OR IGNORE INTO settings VALUES('client',?)");s.bind(1,random);s.step();
    }catch(...){sqlite3_close(db_);db_=nullptr;throw;}
}
CharacterReportStore::~CharacterReportStore(){if(db_)sqlite3_close(db_);}
std::string CharacterReportStore::clientId(){Statement s(db_,"SELECT v FROM settings WHERE k='client'");s.step();return s.text(0);}
bool CharacterReportStore::observe(const std::string& target,const ReportJson& snapshot,int64_t now){
    const auto server=snapshot.at("serverId").get<std::string>(),character=snapshot.at("characterId").get<std::string>();
    sql(db_,"BEGIN IMMEDIATE");try {
        Statement old(db_,"SELECT payload,hash,revision,due,acked FROM reports WHERE target=? AND server=? AND character=?");old.bind(1,target);old.bind(2,server);old.bind(3,character);
        bool exists=old.step();ReportJson fields=exists?ReportJson::parse(old.text(0)):ReportJson::object();auto revision=exists?old.number(2)+1:1;
        ReportJson values=ReportJson::object();for(auto it=fields.begin();it!=fields.end();++it)values[it.key()]=it.value().at("value");
        for(auto it=snapshot.at("values").begin();it!=snapshot.at("values").end();++it){
            if(!values.contains(it.key()) || values[it.key()]!=it.value())fields[it.key()]={{"value",it.value()},{"observedAt",now}};
            values[it.key()]=it.value();
        }
        auto hash=reportHash(values.dump());if(exists && old.text(1)==hash){sql(db_,"COMMIT");return false;}
        if(!exists){Statement count(db_,"SELECT COUNT(*) FROM reports");count.step();if(count.number(0)>=100000)throw ReportCapacityError();}
        // Keep the first deadline while coalescing changes, so busy characters do not starve.
        auto due=exists && old.text(1)!=old.text(4)?std::min(old.number(3),now+3000):now+3000;
        Statement put(db_,"INSERT INTO reports(target,server,character,payload,hash,revision,due) VALUES(?,?,?,?,?,?,?) ON CONFLICT(target,server,character) DO UPDATE SET payload=excluded.payload,hash=excluded.hash,revision=excluded.revision,due=excluded.due,blocked=0");
        put.bind(1,target);put.bind(2,server);put.bind(3,character);put.bind(4,fields.dump());put.bind(5,hash);put.bind(6,revision);put.bind(7,due);put.step();sql(db_,"COMMIT");return true;
    }catch(...){
        // SQLITE_FULL/IOERR may already have rolled back the transaction.
        // Preserve the original error instead of replacing it with "no transaction".
        sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;
    }
}
std::vector<ReportItem> CharacterReportStore::pending(const std::string& target,int64_t now,bool force){
    Statement s(db_,"SELECT server,character,payload,hash,revision FROM reports WHERE target=? AND hash!=acked AND blocked=0 AND due<=? ORDER BY due LIMIT 200");s.bind(1,target);s.bind(2,force?INT64_MAX:now);
    std::vector<ReportItem> result;size_t bytes=0;while(s.step()){auto fields=ReportJson::parse(s.text(2));bytes+=s.text(2).size()+256;if(bytes>450000)break;result.push_back({s.text(0),s.text(1),s.text(3),s.number(4),{{"serverId",s.text(0)},{"characterId",s.text(1)},{"revision",s.number(4)},{"fields",fields}}});}return result;
}
void CharacterReportStore::acknowledge(const std::string& t,const ReportItem& r,int64_t now){
    Statement s(db_,"UPDATE reports SET acked=?,last_success=? WHERE target=? AND server=? AND character=? AND revision=?");s.bind(1,r.hash);s.bind(2,now);s.bind(3,t);s.bind(4,r.server);s.bind(5,r.character);s.bind(6,r.revision);s.step();
}
void CharacterReportStore::fail(const std::string& t,const ReportItem& r,int64_t now,int64_t delay,bool blocked){
    Statement s(db_,"UPDATE reports SET due=?,blocked=? WHERE target=? AND server=? AND character=? AND revision=?");s.bind(1,now+delay);s.bind(2,int64_t(blocked));s.bind(3,t);s.bind(4,r.server);s.bind(5,r.character);s.bind(6,r.revision);s.step();
}
void CharacterReportStore::retry(const std::string& t){Statement s(db_,"UPDATE reports SET due=0,blocked=0 WHERE target=? AND hash!=acked");s.bind(1,t);s.step();}
size_t CharacterReportStore::count(const std::string& t,bool blocked){Statement s(db_,"SELECT COUNT(*) FROM reports WHERE target=? AND hash!=acked AND blocked=?");s.bind(1,t);s.bind(2,int64_t(blocked));s.step();return size_t(s.number(0));}
}
