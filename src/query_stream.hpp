#pragma once
#include "game_protocol.hpp"
#include "jump.hpp"
namespace aion {
struct ObservedFrame { bool outbound{},plaintext{},toolGenerated{}; Bytes wire,plain; };
// Ordered socket bytes only: TCP ordering, retransmission and ACK accounting are
// owned by the two Windows TCP sockets, never simulated by this cipher adapter.
class QueryStream {
public:
    QueryStream(Endpoint source,Endpoint destination,uint32_t firstSequence);
    Bytes feed(std::span<const uint8_t> encrypted);
    std::vector<ObservedFrame> observations;
    void arm(const CipherSnapshot& snapshot);
    // Only after the proxy has completed this connection's RSA handshake.
    void initialize(std::span<const uint8_t> key);
    Bytes query(uint32_t server,uint64_t dbid);
    Bytes guild(bool search,uint8_t order,std::string_view name);
    Bytes jump(const JumpRequest& request);
    Bytes jumpMotion(std::span<const uint8_t> frame);
    bool ready() const {return client_.has_value();}
    bool verified() const {return confirmed_>=3;}
    bool boundary() const {return pending_.empty();}
    bool shifted() const {return shifted_;}
    bool traceable() const {return traceable_;}
    bool takeNativeQuery(){bool v=nativeQuery_;nativeQuery_=false;return v;}
    uint64_t consumed() const {return consumed_;}
private:
    Endpoint source_,destination_;
    uint32_t sequence_;
    Bytes pending_,history_;
    std::optional<CipherSnapshot> client_,server_;
    uint64_t consumed_{};
    size_t confirmed_{};
    bool shifted_{},nativeQuery_{},traceable_=true;
};
}
