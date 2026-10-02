#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
namespace aion {
template<class Record> struct QueryHistory {
    static constexpr size_t Capacity=4096;
    static constexpr int64_t ResultGraceMs=10000;
    std::map<std::string,Record> entries;
    // Keep one slot for a task already in transit when readiness changes.
    bool ready()const{return entries.size()<Capacity-1;}
    bool full()const{return entries.size()>=Capacity;}
    void prune(int64_t now,std::string_view active){
        for(auto it=entries.begin();it!=entries.end();){
            // Expired tasks cannot execute again; retain through result delivery grace.
            if(it->first!=active && it->second.expires<now-ResultGraceMs)it=entries.erase(it);
            else ++it;
        }
    }
};
}
