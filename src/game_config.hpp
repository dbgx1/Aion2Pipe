#pragma once
#include "protocol.hpp"
namespace aion {
struct ConfigValue {std::string path,type,value;};
struct ConfigDocument {
    std::vector<ConfigValue> values;
    size_t expandedBytes{},topLevelKeys{};
    std::string status;
    bool complete{};
};
// 0x3620 data A: u32LE decoded byte length, zlib stream, UTF-16LE JSON.
// Values are identified by their source JSON paths, not invented wire offsets.
ConfigDocument decodeCharacterConfig(std::span<const uint8_t> bytes);
}
