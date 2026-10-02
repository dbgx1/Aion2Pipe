#pragma once
#include "mqtt_codec.hpp"
#include <atomic>
#include <memory>
namespace aion {
struct MqttConfig {
    std::string host="od43e177.ala.cn-shenzhen.emqxsl.cn";
    uint16_t port=8084;
    std::string path="/mqtt",username,password,clientId,topic;
};
// Owned by one background thread. stop may be set by any thread. All network
// handles are asynchronous, so cancellation never closes an active sync request.
class MqttTransport {
public:
    MqttTransport(const MqttConfig& config,const std::atomic<bool>& stop);
    ~MqttTransport();
    MqttTransport(const MqttTransport&)=delete;
    MqttTransport& operator=(const MqttTransport&)=delete;
    std::optional<mqtt::Publication> poll(unsigned timeoutMs=100);
    uint16_t publish(std::string_view topic,std::string_view payload);
    void acknowledge(uint16_t packetId);
    std::vector<uint16_t> acknowledgements();
private: struct Impl;std::unique_ptr<Impl> impl_;
};
}
