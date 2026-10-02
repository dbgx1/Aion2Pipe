#include "query_receipt.hpp"
namespace aion {
QueryOutcome QueryReceipt::snapshot() const {std::lock_guard lock(mutex_);return outcome_;}
void QueryReceipt::sent(){std::lock_guard lock(mutex_);if(outcome_.phase==QueryPhase::Queued)outcome_.phase=QueryPhase::Sent;}
void QueryReceipt::fail(std::string reason){
    std::lock_guard lock(mutex_);
    if(outcome_.phase==QueryPhase::Completed || outcome_.phase==QueryPhase::Failed)return;
    outcome_={QueryPhase::Failed,"unknown",std::move(reason)};
}
void QueryReceipt::complete(const GameMessage& response,bool ambiguous){
    std::lock_guard lock(mutex_);
    if(outcome_.phase!=QueryPhase::Sent)return;
    auto field=[&](const char* name){for(const auto& f:response.fields)if(f.name==name)return f.value;return std::string{};};
    // The verified 0x3650 prefix does not echo the requested database ID. Use
    // serial association only, and reject overlapping native game queries.
    if(ambiguous){outcome_={QueryPhase::Failed,"unknown","native_query_overlap"};return;}
    const auto result=field("资料查询结果（0=成功；其他值待确认）");
    if(result.empty()){
        outcome_={QueryPhase::Failed,"unknown","incomplete_response_prefix"};return;
    }
    if(result!="0"){
        outcome_={QueryPhase::Failed,"unknown","game_query_failed:"+result};return;
    }
    if(!response.viewCharPrefixComplete){
        outcome_={QueryPhase::Failed,"unknown","incomplete_response_prefix"};return;
    }
    const auto online=field("资料角色在线");
    if(online!="true" && online!="false"){
        outcome_={QueryPhase::Failed,"unknown","missing_online_flag"};return;
    }
    outcome_={QueryPhase::Completed,online=="true"?"online":"offline",{}};
}
}
