#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aion {
using Bytes = std::vector<uint8_t>;
struct Endpoint {
    std::array<uint8_t,16> address{}; // IPv4 uses IPv4-mapped IPv6, all in network byte order.
    uint16_t port{};
    auto operator<=>(const Endpoint&) const = default;
    std::string text() const;
    bool wildcard() const;
};
struct Packet {
    // Proxy observations are ordered application frames, not captured IP packets.
    uint64_t proxyConnection{};
    bool plaintext{}, toolGenerated{}, wireObservation{};
    uint64_t id{}, timeUs{};
    uint32_t pid{}, sequence{};
    uint8_t protocol{}, flags{};
    bool outbound{}, attributed{}, ipv6{};
    Endpoint source, destination;
    Bytes raw, payload;
    std::string flowKey() const; // Direction-independent; PID is included to separate process lifetimes.
    std::string directionKey() const;
};
bool parsePacket(std::span<const uint8_t> raw, Packet& packet, std::string& error);
uint64_t readInteger(std::span<const uint8_t> bytes, size_t offset, size_t width, bool bigEndian);
std::optional<Bytes> parseHex(const std::string& text);
std::string hex(std::span<const uint8_t> bytes);
double entropy(std::span<const uint8_t> bytes);

struct Stream {
    Bytes bytes;
    bool initialized{}, startedWithSyn{}, limited{}, closed{};
    uint32_t next{}, initialSequence{};
    size_t duplicateBytes{}, pendingBytes{};
    std::map<uint32_t, Bytes> pending;
    static constexpr size_t Limit = 8 * 1024 * 1024;
    void add(const Packet& packet);
    bool hasGap() const { return !pending.empty(); }
};
struct Framing {
    size_t offset = 0, width = 2;
    bool bigEndian = false, includesHeader = true;
    size_t headerSize = 4, maxFrame = 1024 * 1024, opcodeOffset = 2, opcodeWidth = 2;
    int adjustment = 0;
};
struct Frame { size_t offset{}, length{}; uint64_t opcode{}; bool hasOpcode{}; };
struct FrameResult { std::vector<Frame> frames; size_t consumed{}; std::string status; };
FrameResult splitFrames(std::span<const uint8_t> bytes, const Framing& rule);
void savePcap(const std::filesystem::path& path, const std::vector<Packet>& packets);
void saveSession(const std::filesystem::path& path,const std::vector<Packet>& packets,bool incomplete=false);
std::vector<Packet> loadSession(const std::filesystem::path& path,bool* incomplete=nullptr);
std::vector<Packet> loadPcap(const std::filesystem::path& path);
void saveBinary(const std::filesystem::path& path, std::span<const uint8_t> bytes);
std::vector<Packet> demoPackets();
}
