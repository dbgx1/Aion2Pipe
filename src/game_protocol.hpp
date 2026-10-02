#pragma once
#include "protocol.hpp"
#include "game_config.hpp"
#include "skin.hpp"
namespace aion {
// Direction-specific layouts from 2026-09-24, with core paths rechecked on
// 2026-09-30 and both verified NPC appearance formats supported.
// Verification scope and current limitations: docs/CLIENT_UPDATE_20260930.md.
struct GameFrame { size_t offset{}, length{}, prefixBytes{}; uint32_t encodedLength{}; };
struct GameFrames { std::vector<GameFrame> frames; size_t consumed{}; std::string status; };
GameFrames splitGameFrames(std::span<const uint8_t> bytes, size_t maxFrame=2*1024*1024);
struct GameField {
    size_t offset{}, size{};
    std::string name, type, value, evidence;
    bool meaningKnown{};
    int bit=-1; // Packed booleans can share a byte even across intervening scalar reads.
};
struct GameMessage {
    std::string name, status;
    std::vector<GameField> fields;
    size_t parsedBytes{};
    bool structureComplete{};
    Bytes expanded; // 0xFFFF payload; offsets inside it are not wire offsets.
    ConfigDocument config; // Separate JSON domain: never interpreted as nested game frames.
    std::vector<SkinEquipment> skins;
    bool skinsComplete{};
    std::optional<uint16_t> queryResult; // Available even when a rejected response tail is truncated.
    bool viewCharPrefixComplete{}; // Independent of appearance-tail support.
};
Bytes decompressLz4Block(std::span<const uint8_t> bytes,size_t expected,size_t limit=8*1024*1024);
enum class GameProfile { World, Login };
GameMessage decodeGameFrame(std::span<const uint8_t> bytes, bool plaintext, bool outboundPlaintext=false, GameProfile profile=GameProfile::World);
// Present only on a complete 0x3633 self-appearance message.
std::optional<uint16_t> selfServerId(const GameMessage& message);
// Builds the verified plaintext frame only. Transport must synchronize the live
// connection's cipher and TCP stream; these bytes are not ready for raw injection.
std::string gameQueryResultText(uint16_t result);
Bytes encodeViewCharRequest(uint32_t serverId,uint64_t characterDbid);
Bytes encodeGuildRequest(bool search, uint8_t order=0, std::string_view name={});
struct CipherSnapshot {
    Endpoint source,destination;
    uint32_t frameSequence{};
    uint8_t i{},j{};
    std::array<uint8_t,256> table{};
    void transform(std::span<uint8_t> bytes);
};
CipherSnapshot loadCipherSnapshot(const std::filesystem::path& path);
Bytes decryptGameStream(std::span<const uint8_t> bytes,uint32_t firstSequence,const Endpoint& source,const Endpoint& destination,CipherSnapshot state);
}
