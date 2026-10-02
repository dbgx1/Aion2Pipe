#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <set>
namespace aion {
struct SkinDye {
    uint8_t slot{};
    std::array<uint8_t,3> rgb{},patternRgb{};
    bool remove{},patternRemove{},patternAdditional{};
    uint32_t materialId{},patternId{};
    uint16_t patternColorOpacity{},patternOpacity{},sizeH{},sizeV{},glossy{},metallic{};
    uint8_t patternType{};
};
struct SkinEquipment {
    uint32_t itemId{},skinId{};
    uint8_t visualSlot{};
    bool display{},defaultMesh{},overrideMesh{};
    std::vector<SkinDye> dyes;
};
const char* visualSlotName(uint8_t slot);
std::string skinColorHex(const std::array<uint8_t,3>& color);
struct SkinPreferences {
    std::set<uint32_t> favorites;
    bool prioritize=true;
    void load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
    size_t matches(const std::vector<SkinEquipment>& skins) const;
};
class SkinCatalog {
public:
    void load(const std::filesystem::path& path);
    std::string name(uint32_t id,unsigned race) const;
    std::string source,locale,status;
    size_t size() const {return names_.size();}
private:
    std::map<uint32_t,std::array<std::string,2>> names_;
};
}
