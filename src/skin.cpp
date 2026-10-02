#include "skin.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <windows.h>
namespace aion {
void SkinPreferences::load(const std::filesystem::path& path){
    if(!std::filesystem::exists(path))return;
    if(std::filesystem::file_size(path)>1024*1024)throw std::runtime_error("重点时装设置文件过大");
    std::ifstream in(path);auto j=nlohmann::json::parse(in);
    if(j.at("schema")!=1 || !j.at("favorites").is_array() || j.at("favorites").size()>50000)throw std::runtime_error("重点时装设置格式错误");
    std::set<uint32_t> next;
    for(const auto& v:j.at("favorites")){auto id=v.get<uint64_t>();if(!id || id>UINT32_MAX)throw std::runtime_error("无效重点时装 ID");next.insert(uint32_t(id));}
    auto sort=j.at("prioritize").get<bool>();favorites=std::move(next);prioritize=sort;
}
void SkinPreferences::save(const std::filesystem::path& path) const{
    std::filesystem::create_directories(path.parent_path());auto temp=path;temp+=L".tmp";
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<nlohmann::json{{"schema",1},{"prioritize",prioritize},{"favorites",favorites}}.dump(2);out.flush();if(!out)throw std::runtime_error("无法写入重点时装设置");}
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("无法保存重点时装设置");
}
size_t SkinPreferences::matches(const std::vector<SkinEquipment>& skins) const{
    std::set<uint32_t> found;for(const auto& s:skins)if(s.display && s.skinId && favorites.contains(s.skinId))found.insert(s.skinId);return found.size();
}
const char* visualSlotName(uint8_t slot){
    static constexpr const char* names[]={"无","主手","头盔","肩部","上衣","裤子","手套","鞋子","披风","项链","右耳饰","左耳饰","眼部装饰","嘴部装饰","肩部装饰","头部装饰"};
    return slot<std::size(names)?names[slot]:"未知部位";
}
std::string skinColorHex(const std::array<uint8_t,3>& c){char s[8];snprintf(s,sizeof(s),"#%02X%02X%02X",c[0],c[1],c[2]);return s;}
void SkinCatalog::load(const std::filesystem::path& path){
    names_.clear();source.clear();locale.clear();status.clear();
    try{
        if(std::filesystem::file_size(path)>16*1024*1024)throw std::runtime_error("名称表过大");
        std::ifstream in(path);auto j=nlohmann::json::parse(in);
        if(j.at("schema")!=1 || !j.at("skins").is_array() || j.at("skins").size()>50000)throw std::runtime_error("名称表格式不匹配");
        std::map<uint32_t,std::array<std::string,2>> next;
        for(const auto& s:j.at("skins")){
            auto id=s.at("id").get<uint64_t>();if(!id || id>UINT32_MAX)throw std::runtime_error("无效时装 ID");
            std::array<std::string,2> names{s.at("names").at("light").get<std::string>(),s.at("names").at("dark").get<std::string>()};
            for(const auto& n:names)if(n.size()>4096)throw std::runtime_error("时装名称过长");
            if(!next.emplace(uint32_t(id),std::move(names)).second)throw std::runtime_error("重复时装 ID");
        }
        source=j.at("source").get<std::string>();locale=j.at("locale").get<std::string>();names_=std::move(next);
        status=source+" / "+locale+" / "+std::to_string(names_.size())+" 条";
    }catch(const std::exception&){names_.clear();status="名称表不可用，保留时装 ID";}
}
std::string SkinCatalog::name(uint32_t id,unsigned race) const{
    if(!id)return "未指定时装";
    auto it=names_.find(id);if(it==names_.end())return "名称未收录";
    const auto& n=it->second;
    if(race==1 || race==2)return n[race-1].empty()?"名称未收录":n[race-1];
    if(n[0]==n[1])return n[0].empty()?"名称未收录":n[0];
    return "天族："+(n[0].empty()?std::string("名称未收录"):n[0])+" / 魔族："+(n[1].empty()?std::string("名称未收录"):n[1]);
}
}
