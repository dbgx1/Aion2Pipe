#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <cstdint>
#include <stdexcept>

namespace aion {
// Transport independent batch accumulator: no partial reports are published.
struct QueryBatch {
    using Json=nlohmann::json;
    Json task;
    std::map<std::string,Json> results;
    int64_t expires{},nextPublish{};
    bool acknowledged{};
    explicit QueryBatch(Json value,int64_t now):task(std::move(value)) {
        if(!task.is_object() || task.size()!=5 || task.at("type")!="query_players_online" ||
           !task.at("tasks").is_array() || task.at("tasks").empty() || task.at("tasks").size()>50 ||
           !task.at("expiresAt").is_number_integer())throw std::invalid_argument("Invalid batch");
        expires=task.at("expiresAt").get<int64_t>();
        // Server and client wall clocks are not perfectly synchronized. Allow
        // bounded skew at validation, but never run locally for over 100 s.
        if(expires>now+130000 || expires<now-180000)throw std::invalid_argument("Invalid batch deadline");
        expires=std::min(expires,now+100000);
        std::set<std::string> ids,targets;
        for(const auto& t:task.at("tasks")) {
            if(!t.is_object() || t.size()!=4 || !ids.insert(t.at("attemptId").get<std::string>()).second ||
               !targets.insert(t.at("serverId").get<std::string>()+":"+t.at("characterId").get<std::string>()).second)
                throw std::invalid_argument("Duplicate batch target");
        }
    }
    Json normalized(const Json& t)const {
        auto result=t;result["type"]="query_player_online";result["gameSessionId"]=task.at("gameSessionId");result["expiresAt"]=expires;return result;
    }
    bool done()const{return results.size()==task.at("tasks").size();}
    Json next()const {
        for(const auto& t:task.at("tasks"))if(!results.contains(t.at("attemptId").get<std::string>()))return normalized(t);
        return nullptr;
    }
    void add(Json result,int64_t now) {
        const auto id=result.at("attemptId").get<std::string>();
        result["checkedAt"]=now;results.try_emplace(id,std::move(result));
    }
    void fail(const std::string& reason,int64_t now,const std::set<std::string>* only=nullptr) {
        for(const auto& t:task.at("tasks")) {
            const auto id=t.at("attemptId").get<std::string>();
            if(only && !only->contains(id))continue;
            add(Json{{"type","failed"},{"taskId",t.at("taskId")},{"attemptId",id},{"gameSessionId",task.at("gameSessionId")},{"status","unknown"},{"error",reason}},now);
        }
    }
    Json report()const {
        if(!done())throw std::logic_error("Batch is not complete");
        Json list=Json::array();for(const auto& t:task.at("tasks"))list.push_back(results.at(t.at("attemptId").get<std::string>()));
        return Json{{"type","batch_completed"},{"batchId",task.at("batchId")},{"gameSessionId",task.at("gameSessionId")},{"results",std::move(list)}};
    }
};
}
