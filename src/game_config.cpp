#include "game_config.hpp"
#include "miniz_tinfl.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <stdexcept>
namespace aion {
ConfigDocument decodeCharacterConfig(std::span<const uint8_t> bytes) {
    ConfigDocument result;
    if(bytes.empty()){result.status="没有角色配置数据";return result;}
    try {
        if(bytes.size()<6)throw std::runtime_error("配置压缩头截断");
        const size_t expected=readInteger(bytes,0,4,false);
        if(!expected || expected>8*1024*1024 || expected%2)throw std::runtime_error("配置解压长度无效或超过 8 MiB");
        Bytes expanded(expected);size_t input=bytes.size()-4,output=expected;
        tinfl_decompressor state{};tinfl_init(&state);
        auto status=tinfl_decompress(&state,bytes.data()+4,&input,expanded.data(),expanded.data(),&output,
            TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if(status!=TINFL_STATUS_DONE || input!=bytes.size()-4 || output!=expected)throw std::runtime_error("配置 zlib 校验、长度或尾部不匹配");
        result.expandedBytes=output;
        std::u16string utf16;utf16.reserve(output/2);
        for(size_t i=0;i<output;i+=2)utf16.push_back(char16_t(expanded[i]|(uint16_t(expanded[i+1])<<8)));
        if(!utf16.empty() && utf16.back()==0)utf16.pop_back();
        size_t events=0;
        auto json=nlohmann::ordered_json::parse(utf16,[&](int depth,nlohmann::ordered_json::parse_event_t,nlohmann::ordered_json&){
            if(depth>32 || ++events>16384)throw std::runtime_error("配置 JSON 深度或节点数超过限制");return true;});
        if(!json.is_object())throw std::runtime_error("配置 JSON 顶层不是对象");
        result.topLevelKeys=json.size();
        auto escaped=[](const std::string& key){std::string s;for(char c:key){if(c=='~')s+="~0";else if(c=='/')s+="~1";else s+=c;}return s;};
        std::function<void(const nlohmann::ordered_json&,const std::string&)> visit=[&](const auto& value,const auto& path){
            if(result.values.size()>=4096)throw std::runtime_error("配置字段数量超过限制");
            if(value.is_object() && !value.empty())for(auto it=value.begin();it!=value.end();++it)visit(it.value(),path+"/"+escaped(it.key()));
            else if(value.is_array() && !value.empty())for(size_t i=0;i<value.size();++i)visit(value[i],path+"/"+std::to_string(i));
            else result.values.push_back({path,value.type_name(),value.dump()});
        };
        visit(json,"");result.complete=true;result.status="zlib 校验通过，UTF-16LE JSON 完整解析";
    }catch(const std::exception&){
        result.values.clear();result.status="配置内容未能完整解析（格式、校验或分析上限）；原始字节保留";
    }
    return result;
}
}
