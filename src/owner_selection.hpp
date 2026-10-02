#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace aion {
// A TCP loopback connection has two legitimate owners, one per orientation.
// Conflicting owners on the SAME endpoint remain ambiguous and are rejected.
struct TcpOwnerSelection {
    std::array<uint32_t,2> pid{};
    std::array<bool,2> ambiguous{};
    void add(uint32_t value,bool reverse){
        const size_t i=reverse?1:0;
        if(pid[i] && pid[i]!=value)ambiguous[i]=true;
        if(value)pid[i]=value;
    }
    template<class IsTarget> uint32_t select(IsTarget target,bool& outbound) const {
        uint32_t selected=0;
        for(size_t i=0;i<2;++i){
            if(ambiguous[i])return 0;
            if(!pid[i] || !target(pid[i]))continue;
            if(selected && selected!=pid[i])return 0;
            if(!selected){selected=pid[i];outbound=i==0;}
        }
        return selected;
    }
};
}
