#include "query_batch.hpp"
#include <iostream>
#include <stdexcept>
using aion::QueryBatch;
using Json=nlohmann::json;
void check(bool ok){if(!ok)throw std::runtime_error("batch regression");}
int main(){
    Json tasks=Json::array();
    for(int i=1;i<=50;++i)tasks.push_back({{"taskId","t"+std::to_string(i)},{"attemptId","a"+std::to_string(i)},{"serverId","1005"},{"characterId",std::to_string(i)}});
    Json message={{"type","query_players_online"},{"batchId","batch"},{"gameSessionId","g1"},{"expiresAt",100000},{"tasks",tasks}};
    QueryBatch b(message,0);check(!b.done());
    for(int i=1;i<=49;++i){auto t=b.next();check(t.at("attemptId")=="a"+std::to_string(i));b.add({{"type","completed"},{"taskId",t.at("taskId")},{"attemptId",t.at("attemptId")},{"gameSessionId","g1"},{"status","online"}},i*100);check(!b.done());}
    bool threw=false;try{b.report();}catch(const std::logic_error&){threw=true;}check(threw);
    b.fail("batch_timeout",100000);check(b.done());auto r=b.report();check(r.at("results").size()==50);check(r["results"][0]["status"]=="online");check(r["results"][49]["error"]=="batch_timeout");
    b.fail("cancelled",100001);check(b.report()==r);
    QueryBatch c(message,0);std::set<std::string> cancel={"a1","a3"};c.fail("cancelled",0,&cancel);check(c.next()["attemptId"]=="a2");check(!c.done());c.fail("game_session_changed",5);check(c.done());
    auto invalid=message;invalid["tasks"].push_back(tasks[0]);threw=false;try{QueryBatch bad(invalid,0);}catch(const std::invalid_argument&){threw=true;}check(threw);
    invalid=message;invalid["tasks"][1]=tasks[0];threw=false;try{QueryBatch bad(invalid,0);}catch(const std::invalid_argument&){threw=true;}check(threw);
    invalid=message;invalid["expiresAt"]=103500;QueryBatch skewed(invalid,0);check(skewed.expires==100000);check(skewed.task==invalid);
    invalid=message;invalid["expiresAt"]=130001;threw=false;try{QueryBatch bad(invalid,0);}catch(const std::invalid_argument&){threw=true;}check(threw);
    std::cout<<"PASS batch ordering, no partial reports, 50 limit, partial timeout, cancellation, duplicate results and deadline\n";
}
