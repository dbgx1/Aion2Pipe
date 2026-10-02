#pragma once
#include "protocol.hpp"
#include <deque>
namespace aion::mqtt {
constexpr size_t MaxPacket=65536;
struct Packet {uint8_t header{};Bytes body;};
struct Publication {std::string topic,payload;uint16_t id{};bool retained{},duplicate{};};
Bytes connect(std::string_view clientId,std::string_view username,std::string_view password);
Bytes subscribe(uint16_t id,std::string_view topic);
Bytes publish(uint16_t id,std::string_view topic,std::string_view payload,bool duplicate=false);
Bytes ack(uint16_t id);
Publication publication(const Packet& packet);
class Decoder {
public:
    void feed(std::span<const uint8_t> bytes);
    std::optional<Packet> next();
private: Bytes buffer_;
};
}
