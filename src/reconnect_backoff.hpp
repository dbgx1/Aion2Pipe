#pragma once
#include <algorithm>
#include <cstdint>
namespace aion {
// A brief successful handshake is not evidence of a healthy connection.
class ReconnectBackoff {
public:
    uint32_t failed(uint64_t connectedMs,uint32_t entropy){
        if(connectedMs>=30000)ceiling_=1000;
        const uint32_t floor=ceiling_*3/4;
        const auto delay=floor+entropy%(ceiling_-floor+1);
        ceiling_=std::min<uint32_t>(60000,ceiling_*2);
        return delay;
    }
private:
    uint32_t ceiling_=1000;
};
}
