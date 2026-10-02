#pragma once
#include "game_protocol.hpp"
namespace aion {
enum class ObjectKind {Unknown,Player,Npc,Environment,Mimic};
const char* objectKindName(ObjectKind kind);
struct NearbyObject {
    uint32_t key{};
    ObjectKind kind=ObjectKind::Unknown;
    bool present=true,appearanceSeen=false,positionNeedsRefresh=false,isSelf=false;
    uint64_t firstPacket{},lastPacket{},timeUs{},positionPacket{};
    std::string lastMessage,positionSource;
    std::map<std::string,std::string> values;
    std::vector<SkinEquipment> skins;
    std::optional<std::array<double,3>> position;
    std::optional<std::array<double,3>> networkPosition;
    bool networkPositionNeedsRefresh=false;
};
// One instance per inbound world connection generation. No cross-connection keys.
class NearbyObjects {
public:
    void feed(const Stream& stream,uint64_t packet,uint64_t timeUs,size_t available=SIZE_MAX);
    void message(std::span<const uint8_t> bytes,uint64_t packet,uint64_t timeUs,size_t depth=0);
    bool outbound(std::span<const uint8_t> plaintext,uint64_t packet,uint64_t timeUs);
    const std::map<uint32_t,NearbyObject>& objects() const {return objects_;}
    std::string json(bool includeDeparted) const;
    size_t consumed{},decoded{},unparsed{},updates{},scene{};
    bool gap{},midstream{},closed{},limited{};
    std::string framingStatus,lastUnparsedStatus;
    uint32_t lastUnparsedOpcode{};
private:
    std::map<uint32_t,NearbyObject> objects_;
};
struct StreamObservation {size_t end{};uint64_t packet{},timeUs{};};
// Only the paired inbound model of this directional TCP generation may be passed.
// Consume complete encrypted frames once; never advance the cipher over a gap.
class SelfMovementStream {
public:
    void feed(const Stream& stream,const Endpoint& source,const Endpoint& destination,
              const CipherSnapshot& snapshot,std::span<const StreamObservation> observations,NearbyObjects& model);
    size_t frames{},updates{},incomplete{};
    std::string status="等待同一连接的会话状态与完整数据";
private:
    std::optional<CipherSnapshot> state_;
    size_t consumed_{};
};
}
