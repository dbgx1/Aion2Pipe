#include "game_protocol.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace aion;
static void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
struct Wire {
    Bytes b;size_t bit=8,at=0;
    void u(uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));}
    void var(uint32_t v){do{auto c=uint8_t(v&127);v>>=7;b.push_back(c|(v?128:0));}while(v);}
    void flag(bool v){if(bit==8){at=b.size();b.push_back(0);bit=0;}if(v)b[at]|=uint8_t(1<<bit);++bit;}
};
static Bytes frame(const Bytes& body){Wire w;w.var(uint32_t(body.size()+4));w.b.insert(w.b.end(),body.begin(),body.end());return w.b;}
static void dye(Wire& w,bool remove){
    w.u(remove?2:1,1);w.u(remove?255:0,1);w.u(0,1);w.u(0,1);w.flag(remove);
    w.u(4,4);w.u(123,4);w.u(255,1);w.u(128,1);w.u(17,1);w.flag(!remove);
    for(auto v:{100,200,300,400})w.u(v,2);w.u(2,1);w.flag(true);w.u(500,2);w.u(600,2);
}
static void item(Wire& w,uint32_t id,bool decorated){
    w.u(0,4);w.var(0);w.u(0,8);w.flag(false);
    for(auto n:{1,4,4,1,4,4})w.u(0,n);w.flag(true);w.u(1,1);
    w.var(0);w.u(0,8);w.u(0,1);w.var(0);for(int i=0;i<7;++i)w.var(0);w.u(0,1);
    w.u(4,1);w.u(id,4);w.var(decorated?2:0);if(decorated){dye(w,false);dye(w,true);}
    w.flag(true);w.flag(false);w.flag(true);
}
int main(int argc,char** argv){try{
    check(argc==3,"usage: skin_tests response catalog");
    Wire w;w.u(0x3650,2);w.u(0,2);w.u(0,1);w.u(10,4);
    for(int i=0;i<3;++i)w.u(50,4);w.u(1001,2);w.u(1001,2);w.flag(true);w.u(0,4);
    w.u(0,8);w.var(0);w.u(0,2);w.u(0,1);w.u(0,4);w.u(0,8);w.u(0,2);w.var(0);
    const auto countAt=w.b.size();w.var(2);item(w,10001101,true);item(w,10001102,false);
    auto m=decodeGameFrame(frame(w.b),true);
    check(m.skinsComplete && m.skins.size()==2,"typed skins decoded with shared bool cursor");
    const auto& s=m.skins[0];check(s.itemId==0 && s.skinId==10001101 && s.visualSlot==4 && s.dyes.size()==2,"zero equipment ID preserves actual skin and slot");
    check(s.display && !s.defaultMesh && s.overrideMesh,"display flags after cross-byte dyes");
    check(s.dyes[0].rgb==std::array<uint8_t,3>{0,0,0} && !s.dyes[0].remove,"black is a real color when not removed");
    check(s.dyes[1].remove && s.dyes[1].rgb[0]==255,"remove flag retained independently of RGB");
    check(s.dyes[0].patternRgb==std::array<uint8_t,3>{255,128,17} && s.dyes[0].patternRemove && s.dyes[0].metallic==600,"pattern color and trailing material scalars preserve alignment");
    check(m.skins[1].display && m.skins[1].dyes.empty(),"shared bool cursor continues into second item");
    for(size_t n=2;n<w.b.size();++n){auto truncated=decodeGameFrame(frame(Bytes(w.b.begin(),w.b.begin()+n)),true);check(!truncated.skinsComplete && truncated.skins.empty(),"truncated equipment never published");}
    auto hostile=w.b;hostile[countAt]=127;auto bad=decodeGameFrame(frame(hostile),true);check(!bad.skinsComplete && bad.skins.empty(),"hostile item count bounded");
    std::ifstream in(argv[1],std::ios::binary);check(bool(in),"real fixture exists");Bytes bytes(std::istreambuf_iterator<char>(in),{});auto real=decodeGameFrame(bytes,true);
    check(real.skinsComplete && real.skins.size()==39 && real.parsedBytes==4122,"real 4674-byte response matches independent parser boundary");
    unsigned named=0;bool torso=false,mouth=false;for(const auto& e:real.skins){if(e.skinId)++named;torso|=e.visualSlot==4 && e.skinId==40089201;mouth|=e.visualSlot==13 && e.itemId==0 && e.skinId==70005102;}
    check(named==9 && torso && mouth,"real sample skin IDs and zero-item accessories match evidence");
    SkinCatalog catalog;catalog.load(argv[2]);check(catalog.size()==1463 && catalog.locale=="en-US","exported catalog complete and language explicit");
    check(catalog.name(10001101,1)=="Nameless Master (Greatsword)" && catalog.name(10001101,2)=="Nameless Master (Greatsword)","localized names agree for verified first skin");
    check(catalog.name(40089201,1)=="名称未收录","older region ID not silently mapped to current build");
    check(skinColorHex({255,128,17})=="#FF8011" && skinColorHex({0,0,0})=="#000000","RGB formatting");
    catalog.load("nonexistent-skin-catalog.json");check(catalog.size()==0,"failed reload cannot retain stale names");
    SkinPreferences pref;pref.favorites.insert(10001101);
    check(pref.matches(m.skins)==1,"visible favorite counts once");
    auto duplicates=m.skins;duplicates.push_back(duplicates.front());check(pref.matches(duplicates)==1,"duplicate slots do not inflate priority");
    for(auto& skin:duplicates)skin.display=false;check(pref.matches(duplicates)==0,"hidden skins do not trigger priority");
    auto preferencesPath=std::filesystem::temp_directory_path()/"aion2pipe-skin-preferences-test.json";
    pref.prioritize=false;pref.save(preferencesPath);SkinPreferences restored;restored.load(preferencesPath);
    check(restored.favorites==pref.favorites && !restored.prioritize,"favorites and sort setting survive restart");
    pref.favorites.insert(10001102);pref.save(preferencesPath);restored.load(preferencesPath);
    check(restored.favorites.size()==2,"atomic settings replacement preserves new favorites");std::filesystem::remove(preferencesPath);
    std::cout<<"Skin parser, real response, truncation, shared bool and catalog checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
