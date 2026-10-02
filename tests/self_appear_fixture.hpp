#pragma once
#include "game_protocol.hpp"
// Wire fixture derived from the 0x92BC920 reader and its nested readers.
// Shared bool storage deliberately spans abnormals, equipment and outer fields.
inline aion::Bytes selfAppearFixture(uint32_t mask,uint8_t detailMask=63) {
    aion::Bytes b={0x33,0x36,1};size_t boolAt=0;unsigned boolBit=8;
    auto put=[&](uint64_t n,size_t width){for(size_t i=0;i<width;++i)b.push_back(uint8_t(n>>(8*i)));};
    auto flag=[&](bool v){if(boolBit==8){boolAt=b.size();b.push_back(0);boolBit=0;}if(v)b[boolAt]|=uint8_t(1u<<boolBit);++boolBit;};
    auto str=[&](){b.insert(b.end(),{4,'S','e','l','f'});};
    auto vec=[&](){put(0x3fc00000,4);put(0xc0100000,4);put(0x3e800000,4);};
    constexpr uint64_t wide=0x100000003ULL;
    put(mask,4);put(detailMask,1);if(detailMask&1)str();put(2017,2);put(12,4);if(detailMask&2)put(2,1);
    put(45,4);put(3099,4);put(3100,4);put(50,4);put(wide,8);put(1,4);put(wide,8);if(detailMask&4)put(2,1);
    put(1,1);put(100,2);put(wide,8);put(3,2);put(4,2); // gathering
    put(1,1);put(2,1);put(30,2);put(wide,8);put(5,2); // crafting
    put(10,4);put(20,4);put(wide,8);put(1,1);if(detailMask&8)put(1,1);
    for(unsigned bit=4;bit<6;++bit)if(detailMask&(1u<<bit)){put(1,1);put(1,1);put(9,2);}
    put(0,2);vec();put(0,4);put(0,2);put(1,1);put(100,1);put(127,1);for(int i=0;i<13;++i)put(10,4); // CharState
    if(mask&1){put(wide,8);str();put(3,2);put(1,1);put(2,4);put(wide,8);put(4,2);str();}
    put(1,1);put(1,2);put(0xffffffff,4); // signed stat
    put(1,1);put(0,1);put(1,1);put(1234,4);put(wide,8);put(wide,8);put(1,1);flag(true); // abnormal
    put(1,1);put(5001,4);put(10,1);put(2,1);put(wide,8);flag(false);put(5002,4); // equipment
    put(1,1);for(int i=0;i<4;++i)put(i,1);flag(true);put(4,4);put(5,4);for(int i=0;i<3;++i)put(i,1);flag(false);
    for(int i=0;i<4;++i)put(i+10,2);put(1,1);flag(true);put(20,2);put(30,2);
    put(1,1);put(5003,4);flag(false);flag(true);flag(false);put(1,1); // equipment tail; eight bool bits used
    if(mask&2)put(2,1);put(wide,8);put(101,4);if(mask&4)put(102,4);if(mask&8)put(103,4);if(mask&16)vec();if(mask&32)put(7,2);put(wide,8);
    if(mask&64)put(0x3fc00000,4);if(mask&128)put(123,4);if(mask&256)put(1,1);if(mask&512)put(2,1);if(mask&1024)put(3,1);if(mask&2048)flag(true);if(mask&4096)put(4,1);
    put(2,1);put(0xabcd,2);flag(true);if(mask&8192)put(2,1);if(mask&16384)put(3,1);if(mask&32768)put(456,4);if(mask&65536)put(wide,8);
    if(mask&0x20000){put(1,1);put(1,1);put(2,1);str();put(101,4);vec();put(wide,8);put(wide,8);put(1,1);put(wide,8);}
    for(unsigned bit=18;bit<21;++bit)if(mask&(1u<<bit))put(789,4);if(mask&0x200000)put(wide,8);
    if(mask&0x400000){put(wide,8);put(2018,2);str();put(123,4);put(1,1);}if(mask&0x800000)put(2019,2);
    put(46,4);put(55,4);put(wide,8);if(mask&0x1000000)flag(false);if(mask&0x2000000)put(wide,8);if(mask&0x4000000)put(321,4);
    if(mask&0x8000000)for(int i=0;i<4;++i)put(i+1,4);put(wide,8);put(wide+1,8);
    for(int i=0;i<2;++i){put(1,1);put(1,1);put(519,2);}if(mask&0x10000000){put(1,1);put(1001,4);put(3,4);}if(mask&0x20000000){put(1,1);put(123,4);put(4,4);}
    aion::Bytes result;auto n=uint32_t(b.size()+4);do{auto c=uint8_t(n&127);n>>=7;result.push_back(uint8_t(c|(n?128:0)));}while(n);
    result.insert(result.end(),b.begin(),b.end());return result;
}
